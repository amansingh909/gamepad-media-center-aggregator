#!/usr/bin/env python3
"""GMCA console harness: runs CI artifacts in an emulator on macOS arm64.

Switch builds (.nro) run in Eden, PS Vita builds (.vpk) in Vita3K. The emulator
window stays in the background and never takes the focus:
- Vita3K gets its input from a virtual SDL gamepad loaded into the process
  (vpad.c); SDL hints keep its window inactive;
- Eden gets its input from the DSU ("cemuhook") protocol it listens to, this
  script playing the DSU server; LSUIElement in its Info.plist keeps it inactive.
Captures go through `screencapture -l <window id>`.

The terminal running it needs the macOS "Screen Recording" permission.
Everything downloaded lives in EMU_HOME (default ~/.cache/gmca-emu).

Usage:
  ctl.py setup                        # emulators, Vita firmware (Sony), vpad.dylib
  ctl.py fetch <run-id>               # CI artifacts -> EMU_HOME/artifacts/<run-id>/
  ctl.py launch nx|psvita <artifact>  # run-id or path to .nro/.vpk; --fresh wipes app data,
                                      # --jellyfin starts signed in to the test Jellyfin
  ctl.py key down down a              # up/down/left/right/a/b/x/y/l/r/zl/zr/plus/minus
  ctl.py shot home                    # captures the window -> SHOTS_DIR/home.png
  ctl.py log [lines]                  # last lines of the app log (default 40)
  ctl.py check                        # crash and freeze detection, exit 1 on failure
  ctl.py run scenarios/smoke.txt nx <artifact>   # replays a scenario
  ctl.py quit                         # stops the emulator

Scenario: a text file, one command per line (# = comment); `launch` takes the
platform and artifact given to `run`. Commands: launch [--fresh|--jellyfin],
key, shot, sleep, check, quit.

Buttons are named after the Switch layout, the one borealis uses: a = confirm,
b = back, y = settings. On Vita, a is Cross, b Circle, x Square, y Triangle.
"""

import errno
import glob
import hashlib
import json
import os
import re
import shutil
import signal
import socket
import struct
import subprocess
import sys
import time
import urllib.request
import zipfile
import zlib

ROOT = os.path.dirname(os.path.abspath(__file__))
REPO = "thcolin/gamepad-media-center-aggregator"
EMU_HOME = os.path.expanduser(os.environ.get("EMU_HOME", "~/.cache/gmca-emu"))
DL = os.path.join(EMU_HOME, "dl")
SHOTS_DIR = os.environ.get("SHOTS_DIR", os.path.join(EMU_HOME, "shots"))
STATE = os.path.join(EMU_HOME, "state.json")
KEY_HOLD = float(os.environ.get("KEY_HOLD", "0.12"))
KEY_DELAY = float(os.environ.get("KEY_DELAY", "0.4"))
FREEZE_SECS = float(os.environ.get("FREEZE_SECS", "10"))

# Test server of plenx-media-servers/docker-compose.yml; the password lives in
# the macOS keychain, never in the repository.
JELLYFIN_URL = os.environ.get("JELLYFIN_URL", "http://127.0.0.1:8096")
JELLYFIN_USER = os.environ.get("JELLYFIN_USER", "gmca-emu")
JELLYFIN_KEYCHAIN = "gmca-emu-jellyfin"


# Sony's official download, linked from
# https://www.playstation.com/en-us/support/hardware/psvita/system-software/
VITA_FIRMWARE = ("http://dus01.psv.update.playstation.net/update/psv/image/2022_0209/"
                 "rel_f2c7b12fe85496ec88a0391b514d6e3b/PSVUPDAT.PUP")
VITA_FIRMWARE_SHA256 = "6ef6dc8da6db026f28647713e473486d770087a605c52a8d751bfca7478386cf"
VITA3K_DIR = os.path.join(EMU_HOME, "vita3k")
VITA3K = os.path.join(VITA3K_DIR, "Vita3K.app", "Contents", "MacOS", "Vita3K")
# A "portable" folder next to the bundle keeps config, firmware and apps out of
# ~/Library (vita3k/app/src/app_init.cpp, init_paths).
VITA3K_HOME = os.path.join(VITA3K_DIR, "portable")
VITA_TITLEID = "GMCA00001"  # VITA_TITLEID in CMakeLists.txt
FIFO = os.path.join(EMU_HOME, "vpad.fifo")
VPAD = os.path.join(EMU_HOME, "vpad.dylib")

# SDL_GamepadButton indices (SDL3/SDL_gamepad.h), SDL_GamepadAxis for triggers.
SOUTH, EAST, WEST, NORTH, BACK, START = 0, 1, 2, 3, 4, 6
LSHOULDER, RSHOULDER, UP, DOWN, LEFT, RIGHT = 9, 10, 11, 12, 13, 14
LTRIGGER, RTRIGGER = 4, 5


EDEN_VERSION = "v0.2.1"
EDEN_DMG = f"https://stable.eden-emu.dev/{EDEN_VERSION}/Eden-macOS-{EDEN_VERSION}.dmg"
EDEN_SHA256 = "40575fc79a61550fbb9e332cbae94ccd20f5de20ff61b4b60fe1d42f2525c0b5"
EDEN_DIR = os.path.join(EMU_HOME, "eden")
EDEN = os.path.join(EDEN_DIR, "eden.app", "Contents", "MacOS", "eden")
# A "user" folder next to the bundle is Eden's portable mode.
EDEN_HOME = os.path.join(EDEN_DIR, "user")
EDEN_LOG = os.path.join(EDEN_HOME, "log", "eden_log.txt")
# Logged once the app opened its GPU channel: its first frame follows.
EDEN_READY = "SetChannelPriority"

# DSU protocol (Eden src/input_common/helpers/udp_protocol.h): Eden asks the
# server on port 26760 every 3 s and reads PadData packets from any sender.
DSU_PORT = 26760
DSU_GUID = "0000000000000000000000007f000001"  # UDPClient::GetHostUUID("127.0.0.1")
DSU = {"share": 0x0001, "options": 0x0008, "up": 0x0010, "right": 0x0020, "down": 0x0040,
       "left": 0x0080, "l2": 0x0100, "r2": 0x0200, "l1": 0x0400, "r1": 0x0800,
       "triangle": 0x1000, "circle": 0x2000, "cross": 0x4000, "square": 0x8000}
# Eden's own DSU mapping (UDPClient::GetButtonMappingForDevice): A = Circle.
NX_DSU = {"a": "circle", "b": "cross", "x": "triangle", "y": "square", "l": "l1", "r": "r1",
          "zl": "l2", "zr": "r2", "plus": "options", "minus": "share",
          "up": "up", "down": "down", "left": "left", "right": "right"}
EDEN_INI = {
    "Controls": {
        "enable_udp_controller": "true",
        "udp_input_servers": f"127.0.0.1:{DSU_PORT}",
        **{f"player_0_button_{name}": f'"engine:cemuhookudp,guid:{DSU_GUID},port:{DSU_PORT},'
                                       f'pad:0,button:{DSU[dsu]}"'
           for name, dsu in [("a", "circle"), ("b", "cross"), ("x", "triangle"), ("y", "square"),
                             ("l", "l1"), ("r", "r1"), ("zl", "l2"), ("zr", "r2"),
                             ("plus", "options"), ("minus", "share"), ("dup", "up"),
                             ("ddown", "down"), ("dleft", "left"), ("dright", "right")]},
    },
    # Eden's multi-core JIT crashes GMCA (dynarmic: assert !is_executing).
    "Core": {"use_multi_core": "false"},
    "UI": {"check_for_updates": "false"},
}

PLATFORMS = {
    "psvita": {
        "artifact": "GMCA-Vita-*/GMCA.vpk",
        "log": os.path.join(VITA3K_HOME, "vita3k.log"),
        "app_line": "[export_sceClibPrintf]: ",
        "data": os.path.join(VITA3K_HOME, "fs", "ux0", "data", "GMCA"),
        "fatal": ["|C|", "Unhandled exception", "Segmentation fault"],
    },
    "nx": {
        "artifact": "GMCA-NintendoSwitch-[0-9]*/GMCA.nro",
        # Eden passes program_args the NSO way, libnx reads the homebrew loader
        # argv: GMCA never gets -o, `log` shows Eden's log.
        "log": EDEN_LOG,
        "app_line": "",
        "data": os.path.join(EDEN_HOME, "sdmc", "switch", "GMCA"),
        # svcBreak from the app, or an exception Eden could not handle.
        "fatal": ["broke execution", "Unhandled exception", "Fatal error"],
    },
}


def die(msg):
    sys.exit(f"ctl.py: {msg}")


def load_state():
    try:
        with open(STATE) as f:
            return json.load(f)
    except FileNotFoundError:
        die("aucun émulateur lancé, faire `ctl.py launch` d'abord")


def save_state(state):
    with open(STATE, "w") as f:
        json.dump(state, f)


def alive(pid):
    # Launched by this same process (a scenario), a dead emulator stays a zombie
    # until it is reaped, and kill(pid, 0) still succeeds on a zombie.
    try:
        if os.waitpid(pid, os.WNOHANG)[0] == pid:
            return False
    except ChildProcessError:
        pass
    try:
        os.kill(pid, 0)
        return True
    except OSError:
        return False


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def copy_app(dmg, app, dest_dir):
    mnt = os.path.join(DL, "mnt")
    subprocess.run(["hdiutil", "attach", "-nobrowse", "-readonly", "-quiet", "-mountpoint", mnt, dmg],
                   check=True)
    try:
        os.makedirs(dest_dir, exist_ok=True)
        shutil.copytree(os.path.join(mnt, app), os.path.join(dest_dir, app), symlinks=True)
    finally:
        subprocess.run(["hdiutil", "detach", mnt, "-quiet"])


def setup_vita3k():
    if not os.path.exists(VITA3K):
        # Vita3K only publishes a rolling "continuous" release.
        subprocess.run(["gh", "release", "download", "continuous", "-R", "Vita3K/Vita3K",
                        "-p", "macos-arm64-latest.dmg", "-D", DL, "--clobber"], check=True)
        copy_app(os.path.join(DL, "macos-arm64-latest.dmg"), "Vita3K.app", VITA3K_DIR)
    os.makedirs(VITA3K_HOME, exist_ok=True)
    print(subprocess.run([VITA3K, "-v"], capture_output=True, text=True).stdout.strip().splitlines()[-1])

    if not os.path.isdir(os.path.join(VITA3K_HOME, "fs", "vs0")):
        pup = os.path.join(DL, "PSVUPDAT.PUP")
        if not os.path.exists(pup) or sha256(pup) != VITA_FIRMWARE_SHA256:
            subprocess.run(["curl", "-fsSL", "-o", pup, VITA_FIRMWARE], check=True)
            if sha256(pup) != VITA_FIRMWARE_SHA256:
                die(f"{pup} : empreinte SHA-256 inattendue")
        subprocess.run([VITA3K, "--firmware", pup], check=True,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    print("Vita3K : firmware 3.74 installé")

    sdl = subprocess.run(["brew", "--prefix", "sdl3"], capture_output=True, text=True).stdout.strip()
    if not os.path.isdir(os.path.join(sdl, "include", "SDL3")):
        die("en-têtes SDL3 introuvables : brew install sdl3")
    subprocess.run(["clang", "-dynamiclib", "-arch", "arm64", "-Wall", "-I", os.path.join(sdl, "include"),
                    "-undefined", "dynamic_lookup", "-o", VPAD, os.path.join(ROOT, "vpad.c")], check=True)
    print(f"Vita3K : manette virtuelle {VPAD}")


def write_ini(path, values):
    """Sets keys of a Qt ini file; `key\\default=false` makes Eden read the value."""
    text = open(path).read() if os.path.exists(path) else ""
    for section, keys in values.items():
        if f"[{section}]" not in text:
            text += f"\n[{section}]\n"
        for k, v in keys.items():
            lines = f"{k}\\default=false\n{k}={v}\n"
            pattern = rf"^{re.escape(k)}\\default=.*\n{re.escape(k)}=.*\n"
            if re.search(pattern, text, flags=re.M):
                text = re.sub(pattern, lambda _: lines, text, flags=re.M)
            else:
                text = text.replace(f"[{section}]\n", f"[{section}]\n{lines}", 1)
    with open(path, "w") as f:
        f.write(text)


def setup_eden():
    if not os.path.exists(EDEN):
        dmg = os.path.join(DL, os.path.basename(EDEN_DMG))
        if not os.path.exists(dmg) or sha256(dmg) != EDEN_SHA256:
            subprocess.run(["curl", "-fsSL", "-o", dmg, EDEN_DMG], check=True)
            if sha256(dmg) != EDEN_SHA256:
                die(f"{dmg} : empreinte SHA-256 inattendue")
        copy_app(dmg, "eden.app", EDEN_DIR)
        # An agent app (LSUIElement) is never activated at launch; the edited
        # bundle needs a new ad hoc signature.
        plist = os.path.join(EDEN_DIR, "eden.app", "Contents", "Info.plist")
        subprocess.run(["/usr/libexec/PlistBuddy", "-c", "Add :LSUIElement bool true", plist], check=True)
        subprocess.run(["codesign", "-s", "-", "--force", "--deep", os.path.join(EDEN_DIR, "eden.app")],
                       check=True, capture_output=True)
    os.makedirs(os.path.join(EDEN_HOME, "config"), exist_ok=True)
    write_ini(os.path.join(EDEN_HOME, "config", "qt-config.ini"), EDEN_INI)
    print(f"Eden {EDEN_VERSION} : {EDEN_DIR}")


def setup():
    os.makedirs(DL, exist_ok=True)
    setup_vita3k()
    setup_eden()


def fetch(run_id):
    dest = os.path.join(EMU_HOME, "artifacts", run_id)
    names = subprocess.run(
        ["gh", "api", f"repos/{REPO}/actions/runs/{run_id}/artifacts?per_page=100", "--jq", ".artifacts[].name"],
        capture_output=True, text=True, check=True).stdout.split()
    for name in names:
        if name.startswith(("GMCA-NintendoSwitch-", "GMCA-Vita-")) and "deko3d" not in name:
            target = os.path.join(dest, name)
            if not os.path.isdir(target):
                # Renamed once complete: an interrupted download is not mistaken for one.
                shutil.rmtree(target + ".part", ignore_errors=True)
                subprocess.run(["gh", "run", "download", run_id, "-R", REPO, "-n", name,
                                "-D", target + ".part"], check=True)
                os.rename(target + ".part", target)
    # The app version, so a fresh config can skip the update prompt.
    version_file = os.path.join(dest, "version.txt")
    if not os.path.exists(version_file):
        sha = subprocess.run(["gh", "api", f"repos/{REPO}/actions/runs/{run_id}", "--jq", ".head_sha"],
                             capture_output=True, text=True, check=True).stdout.strip()
        cmake = subprocess.run(["gh", "api", f"repos/{REPO}/contents/CMakeLists.txt?ref={sha}",
                                "-H", "Accept: application/vnd.github.raw"],
                               capture_output=True, text=True, check=True).stdout
        parts = [re.search(rf'set\(VERSION_{p} "(\d+)"\)', cmake) for p in ("MAJOR", "MINOR", "ALTER")]
        if not all(parts):
            die(f"version introuvable dans CMakeLists.txt au commit {sha}")
        with open(version_file, "w") as f:
            f.write(".".join(m.group(1) for m in parts))
    for path in sorted(glob.glob(os.path.join(dest, "*", "GMCA.*"))):
        print(path)


def artifact_path(platform, ref):
    if os.path.isfile(ref):
        return os.path.abspath(ref)
    if not ref.isdigit():
        die(f"fichier introuvable : {ref}")
    pattern = os.path.join(EMU_HOME, "artifacts", ref, PLATFORMS[platform]["artifact"])
    found = glob.glob(pattern)
    if not found:
        fetch(ref)
        found = glob.glob(pattern)
    if not found:
        die(f"aucun artefact {platform} dans le run {ref}")
    return found[0]


def seed_config(data_dir, artifact):
    """A fresh config without the one-time prompts that would swallow the first
    inputs of a scenario: the HOME tile nudge after sign-in on Switch
    (app/src/main.cpp, proposeForwarderInstall) and, when the CI run is known,
    the update prompt, skipped when `app_update` holds the app version."""
    setting = {"hint_forwarder": True, "hint_forwarder_gmca": True}
    version_file = os.path.join(os.path.dirname(os.path.dirname(artifact)), "version.txt")
    if os.path.exists(version_file):
        setting["app_update"] = open(version_file).read().strip()
    config = {"setting": setting}
    os.makedirs(data_dir, exist_ok=True)
    with open(os.path.join(data_dir, "config.json"), "w") as f:
        json.dump(config, f)


def seed_jellyfin(data_dir):
    """Writes the config the Jellyfin sign-in screen would write
    (app/src/tab/jellyfin_add.cpp, JellyfinAdd::finish): neither emulator offers
    an on-screen keyboard usable without the window focus."""
    password = subprocess.run(["security", "find-generic-password", "-s", JELLYFIN_KEYCHAIN,
                               "-a", JELLYFIN_USER, "-w"], capture_output=True, text=True)
    if password.returncode != 0:
        die(f"mot de passe absent du trousseau : item {JELLYFIN_KEYCHAIN}, compte {JELLYFIN_USER}")
    req = urllib.request.Request(
        f"{JELLYFIN_URL}/Users/AuthenticateByName",
        data=json.dumps({"Username": JELLYFIN_USER, "Pw": password.stdout.strip()}).encode(),
        headers={"Content-Type": "application/json",
                 "Authorization": 'MediaBrowser Client="gmca-emu", Device="ctl.py", '
                                  'DeviceId="gmca-emu", Version="1"'})
    opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
    try:
        auth = json.load(opener.open(req, timeout=10))
    except OSError as e:
        die(f"connexion à Jellyfin {JELLYFIN_URL} impossible : {e}")
    server = {"id": auth["ServerId"], "name": "Jellyfin", "access_token": auth["AccessToken"],
              "urls": [JELLYFIN_URL], "type": "jellyfin"}
    user = {"id": auth["User"]["Id"], "name": auth["User"]["Name"],
            "access_token": auth["AccessToken"], "server_id": auth["ServerId"]}
    path = os.path.join(data_dir, "config.json")
    with open(path) as f:
        config = json.load(f)
    config.update({"user_id": user["id"], "users": [user], "servers": [server]})
    with open(path, "w") as f:
        json.dump(config, f)


def start(platform, artifact):
    """Starts the emulator; returns the process and what `key` needs to reach it."""
    out = open(os.path.join(EMU_HOME, f"{platform}.stdout"), "w")
    if platform == "psvita":
        if not os.path.exists(VPAD):
            die("manette virtuelle absente, faire `ctl.py setup`")
        # Installing through Vita3K (a .vpk as content path) brings its window to
        # the front; a .vpk is a zip that Vita3K also accepts unpacked in ux0:app.
        app = os.path.join(VITA3K_HOME, "fs", "ux0", "app", VITA_TITLEID)
        shutil.rmtree(app, ignore_errors=True)
        with zipfile.ZipFile(artifact) as z:
            z.extractall(app)
        if os.path.exists(FIFO):
            os.remove(FIFO)
        os.mkfifo(FIFO)
        env = {**os.environ, "GMCA_VPAD": FIFO, "DYLD_INSERT_LIBRARIES": VPAD,
               "SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS": "1", "SDL_MAC_BACKGROUND_APP": "1",
               "SDL_WINDOW_ACTIVATE_WHEN_SHOWN": "0", "SDL_WINDOW_ACTIVATE_WHEN_RAISED": "0"}
        # -l 2 drops the trace logs.
        cmd = [VITA3K, "--frontend", "sdl", "-l", "2", "-r", VITA_TITLEID]
        return subprocess.Popen(cmd, env=env, stdout=out, stderr=subprocess.STDOUT, start_new_session=True), {}

    if not os.path.exists(EDEN):
        die("Eden absent, faire `ctl.py setup`")
    # The previous log would hold EDEN_READY before Eden rotates it.
    if os.path.exists(EDEN_LOG):
        os.remove(EDEN_LOG)
    dsu = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        dsu.bind(("127.0.0.1", DSU_PORT))
    except OSError as e:
        die(f"port DSU {DSU_PORT} occupé : {e}")
    proc = subprocess.Popen([EDEN, artifact], stdout=out, stderr=subprocess.STDOUT, start_new_session=True)
    # Eden's first request tells the port it receives pad data on.
    dsu.settimeout(30)
    try:
        _, (_, port) = dsu.recvfrom(128)
    except socket.timeout:
        proc.kill()
        die("Eden n'a envoyé aucune requête DSU en 30 s")
    finally:
        dsu.close()
    return proc, {"dsu_port": port}


def launch(platform, ref, fresh=False, jellyfin=False):
    quit_emu()
    conf = PLATFORMS[platform]
    artifact = artifact_path(platform, ref)
    if fresh or jellyfin:
        shutil.rmtree(conf["data"], ignore_errors=True)
        seed_config(conf["data"], artifact)
    if jellyfin:
        seed_jellyfin(conf["data"])
    proc, extra = start(platform, artifact)
    save_state({"platform": platform, "pid": proc.pid, **extra})
    for _ in range(90):
        if not alive(proc.pid):
            quit_emu()
            die(f"l'émulateur s'est arrêté au lancement, voir {EMU_HOME}/{platform}.stdout")
        if window_id(proc.pid) and (platform != "nx" or ready_nx()):
            break
        time.sleep(1)
    else:
        quit_emu()
        die("l'app ne s'est pas affichée après 90 s")
    print(f"{platform} : pid {proc.pid}, {artifact}")


def ready_nx():
    try:
        with open(EDEN_LOG, errors="replace") as f:
            return EDEN_READY in f.read()
    except FileNotFoundError:
        return False


def quit_emu():
    try:
        with open(STATE) as f:
            pid = json.load(f)["pid"]
    except FileNotFoundError:
        # An emulator left over by an interrupted run would hold the FIFO or the window.
        for binary in (VITA3K, EDEN):
            subprocess.run(["pkill", "-9", "-f", binary])
        return
    # A state left by a reboot may name a PID reused by another process.
    comm = subprocess.run(["ps", "-o", "comm=", "-p", str(pid)], capture_output=True, text=True).stdout.strip()
    if alive(pid) and comm in (VITA3K, EDEN):
        os.killpg(pid, signal.SIGTERM)
        for _ in range(20):
            if not alive(pid):
                break
            time.sleep(0.25)
        else:
            # Eden ignores SIGTERM.
            os.killpg(pid, signal.SIGKILL)
    os.remove(STATE)


# JXA reads the window list without activating anything; pyobjc is not needed.
WINDOW_LIST_JS = """
ObjC.import('CoreGraphics');
function run(argv) {
  const list = ObjC.deepUnwrap(ObjC.castRefToObject(
    $.CGWindowListCopyWindowInfo($.kCGWindowListOptionAll, 0)));
  const wins = list.filter(w => w.kCGWindowOwnerPID == argv[0] && w.kCGWindowLayer == 0
                            && w.kCGWindowIsOnscreen && w.kCGWindowBounds.Height > 100);
  return wins.length ? String(wins[0].kCGWindowNumber) : '';
}
"""


def window_id(pid):
    r = subprocess.run(["osascript", "-l", "JavaScript", "-e", WINDOW_LIST_JS, str(pid)],
                       capture_output=True, text=True)
    return int(r.stdout.strip()) if r.stdout.strip() else None


def capture(path):
    wid = window_id(load_state()["pid"])
    if not wid:
        die("fenêtre de l'émulateur introuvable")
    os.makedirs(os.path.dirname(path), exist_ok=True)
    # -o: no window shadow, -x: no sound.
    subprocess.run(["screencapture", "-x", "-o", "-l", str(wid), path], check=True)
    return path


def shot(name):
    print(capture(os.path.join(SHOTS_DIR, f"{name}.png")))


def vpad_send(line):
    # A blocking open would wait forever if vpad.dylib never opened the FIFO.
    for _ in range(50):
        if not alive(load_state()["pid"]):
            die("l'émulateur ne tourne plus")
        try:
            fd = os.open(FIFO, os.O_WRONLY | os.O_NONBLOCK)
            break
        except OSError as e:
            if e.errno != errno.ENXIO:
                raise
            time.sleep(0.1)
    else:
        die(f"la manette virtuelle ne lit pas {FIFO}, voir les lignes vpad: de {EMU_HOME}/psvita.stdout")
    with os.fdopen(fd, "w") as f:
        f.write(line + "\n")


def vita_press(name):
    # Borealis reads Square as X and Triangle as Y (platforms/psv/psv_input.cpp).
    buttons = {"a": SOUTH, "b": EAST, "x": WEST, "y": NORTH, "l": LSHOULDER, "r": RSHOULDER,
               "plus": START, "minus": BACK, "up": UP, "down": DOWN, "left": LEFT, "right": RIGHT}
    if name in ("zl", "zr"):
        axis = LTRIGGER if name == "zl" else RTRIGGER
        vpad_send(f"axis {axis} 32767")
        time.sleep(KEY_HOLD)
        # A virtual gamepad trigger rests at the axis minimum.
        vpad_send(f"axis {axis} -32768")
    else:
        # Vita3K maps SDL buttons by position: south = Cross, north = Triangle.
        vpad_send(f"button {buttons[name]} 1")
        time.sleep(KEY_HOLD)
        vpad_send(f"button {buttons[name]} 0")


def dsu_packet(buttons):
    """A DSU PadData message (Response::PadData, 80 bytes) for pad 0."""
    counter = int(time.monotonic() * 1000) & 0xFFFFFFFF  # Eden drops a repeated counter
    data = struct.pack("<BBBB6sBBIHBB4B12B12xQ6f",
                       0, 2, 2, 1, b"\0" * 6, 0x05, 1,  # pad 0, connected, full gyro, usb, full battery
                       counter, buttons, 0, 0,
                       128, 128, 128, 128,              # sticks at rest
                       *([0] * 12), 0, *([0.0] * 6))
    header = struct.pack("<IHHIII", 0x53555344, 1001, len(data) + 4, 0, 0, 0x00100002)
    crc = zlib.crc32(header + data)
    return header[:8] + struct.pack("<I", crc) + header[12:] + data


def nx_press(name):
    state = load_state()
    if not alive(state["pid"]):
        die("l'émulateur ne tourne plus")
    mask = DSU[NX_DSU[name]]
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
        s.sendto(dsu_packet(mask), ("127.0.0.1", state["dsu_port"]))
        time.sleep(KEY_HOLD)
        s.sendto(dsu_packet(0), ("127.0.0.1", state["dsu_port"]))


def key(names):
    press = nx_press if load_state()["platform"] == "nx" else vita_press
    for name in names:
        name = name.lower()
        if name not in NX_DSU:
            die(f"bouton inconnu : {name}")
        press(name)
        time.sleep(KEY_DELAY)


def log(n=40):
    conf = PLATFORMS[load_state()["platform"]]
    try:
        with open(conf["log"], errors="replace") as f:
            lines = [l.split(conf["app_line"], 1)[1] for l in f if conf["app_line"] in l]
    except FileNotFoundError:
        die(f"pas encore de log : {conf['log']}")
    sys.stdout.writelines(lines[-n:])


def crash():
    state = load_state()
    conf = PLATFORMS[state["platform"]]
    if not alive(state["pid"]):
        return "l'émulateur s'est arrêté"
    try:
        with open(conf["log"], errors="replace") as f:
            fatal = [l.strip() for l in f if any(p in l for p in conf["fatal"])]
    except FileNotFoundError:
        return None
    return fatal[-1] if fatal else None


def check():
    """Crash, then freeze: the window and the logs stay identical for
    FREEZE_SECS. An idle screen also stays identical, so `check` belongs right
    after an input that must keep the screen moving, a playback for instance."""
    reason = crash()
    if reason:
        print("PLANTAGE : " + reason)
        return False
    conf = PLATFORMS[load_state()["platform"]]

    tmp = os.path.join(EMU_HOME, "check")
    before, size = sha256(capture(tmp + "-a.png")), os.path.getsize(conf["log"])
    time.sleep(FREEZE_SECS)
    if sha256(capture(tmp + "-b.png")) == before and os.path.getsize(conf["log"]) == size:
        print(f"GEL : écran et logs figés pendant {FREEZE_SECS:g} s")
        return False
    print("OK")
    return True


def run_scenario(path, platform, ref):
    ok = True
    with open(path) as f:
        for n, raw in enumerate(f, 1):
            line = raw.split("#", 1)[0].strip()
            if not line:
                continue
            cmd, *args = line.split()
            print(f"{n:3} {line}", flush=True)
            try:
                if cmd == "launch":
                    launch(platform, ref, fresh="--fresh" in args, jellyfin="--jellyfin" in args)
                elif cmd == "quit":
                    quit_emu()
                elif cmd == "key":
                    key(args)
                elif cmd == "shot":
                    shot(f"{platform}-{args[0]}")
                elif cmd == "sleep":
                    time.sleep(float(args[0]))
                elif cmd == "check":
                    ok = check() and ok
                else:
                    die(f"commande inconnue ligne {n} : {line}")
            except SystemExit as e:
                print(e, flush=True)
                quit_emu()
                ok = False
                break
            reason = crash() if cmd != "quit" and os.path.exists(STATE) else None
            if reason:
                if cmd != "check":  # check already printed it
                    print("PLANTAGE : " + reason)
                ok = False
                break
    print("VERDICT : " + ("OK" if ok else "ÉCHEC"))
    return ok


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    cmd, *args = sys.argv[1:]
    if cmd == "setup":
        setup()
    elif cmd == "fetch":
        fetch(args[0])
    elif cmd == "launch":
        rest = [a for a in args if not a.startswith("--")]
        launch(rest[0], rest[1], fresh="--fresh" in args, jellyfin="--jellyfin" in args)
    elif cmd == "quit":
        quit_emu()
    elif cmd == "key":
        key(args)
    elif cmd == "shot":
        shot(args[0])
    elif cmd == "log":
        log(int(args[0]) if args else 40)
    elif cmd == "check":
        sys.exit(0 if check() else 1)
    elif cmd == "run":
        sys.exit(0 if run_scenario(*args[:3]) else 1)
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main()
