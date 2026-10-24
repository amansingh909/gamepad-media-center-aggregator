#!/usr/bin/env python3
"""GMCA console harness: runs CI artifacts in an emulator on macOS arm64.

Switch builds (.nro) run in Ryubing, PS Vita builds (.vpk) in Vita3K. The
emulator window stays in the background: input goes through a virtual SDL
gamepad loaded into the emulator (vpad.c), captures through
`screencapture -l <window id>`. Nothing here activates a window.

The terminal running it needs the macOS "Screen Recording" permission.
Everything downloaded lives in EMU_HOME (default ~/.cache/gmca-emu).

Usage:
  ctl.py setup                     # emulators, Vita firmware (Sony), vpad.dylib
  ctl.py fetch <run-id>            # CI artifacts -> EMU_HOME/artifacts/<run-id>/
  ctl.py launch psvita <artifact>  # run-id or path to .vpk/.nro; --fresh wipes app data
  ctl.py key down down a           # up/down/left/right/a/b/x/y/l/r/zl/zr/plus/minus
  ctl.py shot home                 # captures the window -> SHOTS_DIR/home.png
  ctl.py log [lines]               # app lines of the emulator log (default 40)
  ctl.py check                     # crash and freeze detection, exit 1 on failure
  ctl.py run scenarios/smoke.txt   # replays a scenario
  ctl.py quit                      # stops the emulator

Scenario: a text file, one command per line (# = comment). `launch` takes the
platform and artifact from the command line of `run`:
  ctl.py run scenarios/smoke.txt psvita 40083100934
Commands: launch [--fresh], key, shot, sleep, check, quit.

Buttons are named after the Switch layout, the one borealis uses: a = confirm,
b = back, x = context menu. On Vita, a is Cross and b is Circle.
"""

import glob
import hashlib
import json
import os
import shutil
import signal
import subprocess
import sys
import time
import zipfile

ROOT = os.path.dirname(os.path.abspath(__file__))
REPO = "thcolin/gamepad-media-center-aggregator"
EMU_HOME = os.path.expanduser(os.environ.get("EMU_HOME", "~/.cache/gmca-emu"))
SHOTS_DIR = os.environ.get("SHOTS_DIR", os.path.join(EMU_HOME, "shots"))
STATE = os.path.join(EMU_HOME, "state.json")
FIFO = os.path.join(EMU_HOME, "vpad.fifo")
VPAD = os.path.join(EMU_HOME, "vpad.dylib")
KEY_HOLD = float(os.environ.get("KEY_HOLD", "0.12"))
KEY_DELAY = float(os.environ.get("KEY_DELAY", "0.4"))
FREEZE_SECS = float(os.environ.get("FREEZE_SECS", "10"))

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

# SDL_GamepadButton indices (SDL3/SDL_gamepad.h).
SOUTH, EAST, WEST, NORTH, BACK, GUIDE, START = 0, 1, 2, 3, 4, 5, 6
LSHOULDER, RSHOULDER, UP, DOWN, LEFT, RIGHT = 9, 10, 11, 12, 13, 14
# SDL_GamepadAxis indices for the triggers.
LTRIGGER, RTRIGGER = 4, 5

COMMON_BUTTONS = {"up": UP, "down": DOWN, "left": LEFT, "right": RIGHT,
                  "l": LSHOULDER, "r": RSHOULDER, "plus": START, "minus": BACK}

PLATFORMS = {
    "psvita": {
        "artifact": "GMCA-Vita-*/GMCA.vpk",
        "log": os.path.join(VITA3K_HOME, "vita3k.log"),
        "data": os.path.join(VITA3K_HOME, "fs", "ux0", "data", "GMCA"),
        # Vita3K maps SDL buttons by position: south = Cross, north = Triangle.
        "buttons": {**COMMON_BUTTONS, "a": SOUTH, "b": EAST, "x": NORTH, "y": WEST},
        "app_line": "[export_sceClibPrintf]: ",
        "fatal": ["|C|", "Unhandled exception", "Segmentation fault"],
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


def alive(pid):
    try:
        os.kill(pid, 0)
        return True
    except OSError:
        return False


# --- setup -----------------------------------------------------------------

def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def setup_vita3k():
    dl = os.path.join(EMU_HOME, "dl")
    os.makedirs(dl, exist_ok=True)
    if not os.path.exists(VITA3K):
        # Vita3K only publishes a rolling "continuous" release.
        subprocess.run(["gh", "release", "download", "continuous", "-R", "Vita3K/Vita3K",
                        "-p", "macos-arm64-latest.dmg", "-D", dl, "--clobber"], check=True)
        mnt = os.path.join(dl, "vita3k-mnt")
        subprocess.run(["hdiutil", "attach", "-nobrowse", "-readonly", "-quiet", "-mountpoint", mnt,
                        os.path.join(dl, "macos-arm64-latest.dmg")], check=True)
        try:
            os.makedirs(VITA3K_DIR, exist_ok=True)
            shutil.copytree(os.path.join(mnt, "Vita3K.app"), os.path.join(VITA3K_DIR, "Vita3K.app"),
                            symlinks=True)
        finally:
            subprocess.run(["hdiutil", "detach", mnt, "-quiet"])
    os.makedirs(VITA3K_HOME, exist_ok=True)
    print(subprocess.run([VITA3K, "-v"], capture_output=True, text=True).stdout.strip().splitlines()[-1])

    if not os.path.isdir(os.path.join(VITA3K_HOME, "fs", "vs0")):
        pup = os.path.join(dl, "PSVUPDAT.PUP")
        if not os.path.exists(pup) or sha256(pup) != VITA_FIRMWARE_SHA256:
            subprocess.run(["curl", "-fsSL", "-o", pup, VITA_FIRMWARE], check=True)
            if sha256(pup) != VITA_FIRMWARE_SHA256:
                die(f"{pup} : empreinte SHA-256 inattendue")
        subprocess.run([VITA3K, "--firmware", pup], check=True,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    print("Vita3K : firmware 3.74 installé")


def setup_vpad():
    sdl = subprocess.run(["brew", "--prefix", "sdl3"], capture_output=True, text=True).stdout.strip()
    if not os.path.isdir(os.path.join(sdl, "include", "SDL3")):
        die("en-têtes SDL3 introuvables : brew install sdl3")
    subprocess.run(["clang", "-dynamiclib", "-arch", "arm64", "-Wall", "-I", os.path.join(sdl, "include"),
                    "-undefined", "dynamic_lookup", "-o", VPAD, os.path.join(ROOT, "vpad.c")], check=True)
    print(f"manette virtuelle : {VPAD}")


def setup():
    os.makedirs(EMU_HOME, exist_ok=True)
    setup_vpad()
    setup_vita3k()


# --- artifacts -------------------------------------------------------------

def fetch(run_id):
    dest = os.path.join(EMU_HOME, "artifacts", run_id)
    names = subprocess.run(
        ["gh", "api", f"repos/{REPO}/actions/runs/{run_id}/artifacts", "--jq", ".artifacts[].name"],
        capture_output=True, text=True, check=True).stdout.split()
    for name in names:
        if name.startswith(("GMCA-NintendoSwitch-", "GMCA-Vita-")) and "deko3d" not in name:
            if not os.path.isdir(os.path.join(dest, name)):
                subprocess.run(["gh", "run", "download", run_id, "-R", REPO, "-n", name,
                                "-D", os.path.join(dest, name)], check=True)
    for path in sorted(glob.glob(os.path.join(dest, "*", "GMCA.*"))):
        print(path)


def artifact_path(platform, ref):
    if os.path.isfile(ref):
        return os.path.abspath(ref)
    pattern = os.path.join(EMU_HOME, "artifacts", ref, PLATFORMS[platform]["artifact"])
    found = glob.glob(pattern)
    if not found:
        fetch(ref)
        found = glob.glob(pattern)
    if not found:
        die(f"aucun artefact {platform} dans le run {ref}")
    return found[0]


# --- emulator --------------------------------------------------------------

def command(platform, artifact):
    if platform == "psvita":
        # Installing through Vita3K (a .vpk as content path) brings its window to
        # the front; a .vpk is a zip that Vita3K also accepts unpacked in ux0:app.
        app = os.path.join(VITA3K_HOME, "fs", "ux0", "app", VITA_TITLEID)
        shutil.rmtree(app, ignore_errors=True)
        with zipfile.ZipFile(artifact) as z:
            z.extractall(app)
        # -l 2 drops the trace logs.
        return [VITA3K, "--frontend", "sdl", "-l", "2", "-r", VITA_TITLEID]
    die(f"plateforme inconnue : {platform}")


def launch(platform, ref, fresh=False):
    quit_emu()
    conf = PLATFORMS[platform]
    artifact = artifact_path(platform, ref)
    if fresh:
        shutil.rmtree(conf["data"], ignore_errors=True)
    if not os.path.exists(VPAD):
        die("manette virtuelle absente, faire `ctl.py setup`")
    if os.path.exists(FIFO):
        os.remove(FIFO)
    os.mkfifo(FIFO)
    # The SDL hints keep the emulator from taking the focus, at launch included.
    env = {**os.environ, "GMCA_VPAD": FIFO, "DYLD_INSERT_LIBRARIES": VPAD,
           "SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS": "1", "SDL_MAC_BACKGROUND_APP": "1",
           "SDL_WINDOW_ACTIVATE_WHEN_SHOWN": "0", "SDL_WINDOW_ACTIVATE_WHEN_RAISED": "0"}
    out = open(os.path.join(EMU_HOME, f"{platform}.stdout"), "w")
    proc = subprocess.Popen(command(platform, artifact), env=env, stdout=out, stderr=subprocess.STDOUT,
                            start_new_session=True)
    state = {"platform": platform, "pid": proc.pid, "artifact": artifact, "started": time.time()}
    with open(STATE, "w") as f:
        json.dump(state, f)
    for _ in range(60):
        if not alive(proc.pid):
            die(f"l'émulateur s'est arrêté au lancement, voir {out.name}")
        if window_id(proc.pid):
            break
        time.sleep(1)
    else:
        die("aucune fenêtre après 60 s")
    print(f"{platform} : pid {proc.pid}, {artifact}")


def quit_emu():
    try:
        with open(STATE) as f:
            pid = json.load(f)["pid"]
    except FileNotFoundError:
        return
    if alive(pid):
        os.killpg(pid, signal.SIGTERM)
        for _ in range(20):
            if not alive(pid):
                break
            time.sleep(0.25)
        else:
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
    state = load_state()
    wid = window_id(state["pid"])
    if not wid:
        die("fenêtre de l'émulateur introuvable")
    os.makedirs(os.path.dirname(path), exist_ok=True)
    # -o: no window shadow, -x: no sound.
    subprocess.run(["screencapture", "-x", "-o", "-l", str(wid), path], check=True)
    return path


def shot(name):
    print(capture(os.path.join(SHOTS_DIR, f"{name}.png")))


def send(line):
    # Opening the FIFO blocks until vpad.dylib reads it; a dead emulator would hang here.
    if not alive(load_state()["pid"]):
        die("l'émulateur ne tourne plus")
    with open(FIFO, "w") as f:
        f.write(line + "\n")


def key(names):
    buttons = PLATFORMS[load_state()["platform"]]["buttons"]
    for name in names:
        name = name.lower()
        if name in ("zl", "zr"):
            axis = LTRIGGER if name == "zl" else RTRIGGER
            send(f"axis {axis} 32767")
            time.sleep(KEY_HOLD)
            send(f"axis {axis} 0")
        elif name in buttons:
            send(f"button {buttons[name]} 1")
            time.sleep(KEY_HOLD)
            send(f"button {buttons[name]} 0")
        else:
            die(f"bouton inconnu : {name}")
        time.sleep(KEY_DELAY)


def app_lines(n):
    state = load_state()
    conf = PLATFORMS[state["platform"]]
    with open(conf["log"], errors="replace") as f:
        lines = [l.split(conf["app_line"], 1)[1] for l in f if conf["app_line"] in l]
    return lines[-n:]


def log(n=40):
    sys.stdout.writelines(app_lines(n))


def check():
    """Crash: the emulator died or logged a fatal error. Freeze: the window and
    the log stay identical for FREEZE_SECS. An idle screen also stays identical,
    so `check` belongs right after an input that must change the screen."""
    state = load_state()
    conf = PLATFORMS[state["platform"]]
    if not alive(state["pid"]):
        print("PLANTAGE : l'émulateur s'est arrêté")
        return False
    with open(conf["log"], errors="replace") as f:
        fatal = [l.strip() for l in f if any(p in l for p in conf["fatal"])]
    if fatal:
        print("PLANTAGE : " + fatal[-1])
        return False
    tmp = os.path.join(EMU_HOME, "check")
    before, size = sha256(capture(tmp + "-a.png")), os.path.getsize(conf["log"])
    time.sleep(FREEZE_SECS)
    if sha256(capture(tmp + "-b.png")) == before and os.path.getsize(conf["log"]) == size:
        print(f"GEL : écran et log figés pendant {FREEZE_SECS:g} s")
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
            if cmd == "launch":
                launch(platform, ref, fresh="--fresh" in args)
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
            if cmd != "quit" and os.path.exists(STATE) and not alive(load_state()["pid"]):
                print("PLANTAGE : l'émulateur s'est arrêté")
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
        rest = [a for a in args if a != "--fresh"]
        launch(rest[0], rest[1], fresh="--fresh" in args)
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
