# Console emulation harness

`ctl.py` runs the Switch (`.nro`) and PS Vita (`.vpk`) builds of a CI run in an emulator on
macOS arm64, so an agent can launch the app, navigate with a gamepad, capture the screen, read
the logs and catch a crash or a freeze without a console.

| Platform (`ctl.py`) | CI artifact | Emulator | Input | `log` shows |
|---|---|---|---|---|
| `nx` | `GMCA-NintendoSwitch-<n>/GMCA.nro` | [Eden](https://git.eden-emu.dev/eden-emu/eden) v0.2.1, pinned by SHA-256 | DSU over UDP | Eden's log |
| `psvita` | `GMCA-Vita-<n>/GMCA.vpk` | [Vita3K](https://github.com/Vita3K/Vita3K), rolling `continuous` release, not pinned | virtual gamepad (`vpad.dylib`) | the app's lines of `vita3k.log` |

Both emulators stay in the background and never take the focus:

- **Eden** listens for gamepad input over DSU, the UDP protocol of the cemuhook tools.
  `launch` binds UDP port 26760 (it must be free) until Eden's first request tells which port to
  send to, then `key` sends DSU packets there. Eden runs as a background app without a Dock icon
  (`LSUIElement` set in its `Info.plist`).
- **Vita3K** gets a virtual gamepad: `setup` compiles `vpad.c` into `vpad.dylib`, `launch`
  loads it into Vita3K with `DYLD_INSERT_LIBRARIES`, and `key` writes commands to a FIFO it
  reads. SDL environment variables (`SDL_MAC_BACKGROUND_APP`, `SDL_WINDOW_ACTIVATE_WHEN_SHOWN`)
  keep the window inactive.

Captures use `screencapture -l <window id>`.

## Setup

Requirements: `gh` signed in, `brew install sdl3` (headers for `vpad.c`), and the
"Screen Recording" permission for the terminal.

```bash
scripts/emu/ctl.py setup
```

Everything lands in `~/.cache/gmca-emu` (`EMU_HOME`), private to the user. Both emulators run in
portable mode, their settings and data in a folder next to the app instead of `~/Library`:
`eden/user` and `vita3k/portable`. The Vita firmware 3.74 comes from Sony's update server
(`dus01.psv.update.playstation.net`, linked from playstation.com) and is checked by SHA-256.
Neither emulator needs console keys (`prod.keys`) or a firmware dump: Eden synthesizes the
Switch shared fonts, the Vita app ships its own.

Ryubing, the maintained Ryujinx fork, was the first choice for Switch. Its servers
(ryujinx.app, git.ryujinx.app) have been unreachable since September 2026.

## Use

A CI run is a GitHub Actions run of `build.yaml`, its id is the number in the run's URL.

```bash
scripts/emu/ctl.py fetch 40083100934                  # downloads the run's .nro and .vpk
scripts/emu/ctl.py launch nx 40083100934 --fresh      # a run id, or a path to a .nro/.vpk
scripts/emu/ctl.py key a                              # up/down/left/right/a/b/x/y/l/r/zl/zr/plus/minus
scripts/emu/ctl.py shot home                          # SHOTS_DIR/home.png
scripts/emu/ctl.py log 20
scripts/emu/ctl.py check
scripts/emu/ctl.py quit

scripts/emu/ctl.py run scripts/emu/scenarios/smoke.txt psvita 40083100934
python3 scripts/emu/test_ctl.py                       # checks the DSU packet Eden validates
```

`fetch` refuses a run built from a fork, whose code would run on this Mac and whose log would
reach the agent reading it; `EMU_UNTRUSTED=1` accepts it. A local `.nro` or `.vpk` given to
`launch` runs as is. A CI run without a PR: `gh workflow run build.yaml -R
thcolin/gamepad-media-center-aggregator --ref <branch> -f platform=nx` (or `psvita`), one run per
platform.

Buttons follow the Switch layout borealis uses: `a` confirms, `b` goes back, `y` opens the
settings, `plus` and `minus` are Start and Select on Vita. On Vita, `a` is Cross, `b` Circle,
`x` Square, `y` Triangle; `zl` and `zr` only matter on Switch.

`launch --fresh` wipes the app data and writes a config without the one-time prompts: the Switch
prompt to install a HOME menu tile, and the update prompt when the build comes from a run id,
whose version `fetch` reads in `CMakeLists.txt`. `launch --jellyfin` also signs in to the test
Jellyfin, which must be running (`plenx-media-servers/docker-compose.yml`, see `CLAUDE.md`),
with the `gmca-emu` account. Its password is in the keychain item `gmca-emu-jellyfin`
(`security add-generic-password -s gmca-emu-jellyfin -a gmca-emu -w`). Neither emulator offers
an on-screen keyboard usable without the window focus, so the sign-in screen itself is not
exercised.

Environment: `EMU_HOME`, `SHOTS_DIR` (default `EMU_HOME/shots`), `KEY_HOLD` and `KEY_DELAY`
(seconds a button is held, and waited after), `FREEZE_SECS` (default 10), `EMU_UNTRUSTED`,
`JELLYFIN_URL`, `JELLYFIN_USER`.

## Scenarios

A scenario is a text file, one command per line: `launch [--fresh|--jellyfin]`, `key`, `shot`,
`sleep`, `check`, `quit`. The platform and the build come from `run`; captures are named
`<platform>-<name>.png`.

- `smoke.txt`: fresh config, Connections, connection types, settings. No network.
- `jellyfin.txt`: signed in, home, the Movies library, its first movie, ten seconds of playback
  with `check`, back. On `nx` the playback shows Eden's socket error, see below.

The output ends on `VERDICT : OK` or `VERDICT : ÉCHEC`, and stops early on `PLANTAGE` (crash:
the emulator died, or its log holds a fatal error). `check` also reports `GEL` (freeze) when the
screen and the log stay identical for `FREEZE_SECS`; an idle screen is identical too, so `check`
belongs after an input that keeps the screen moving, like a playback. An error dialog is neither
a crash nor a freeze: the captures are there to be read.

## What an emulator does not catch

- **Socket constants of OpenOrbis**, the PS4 toolchain: PS4 is out of scope, and a wrong
  `MSG_NOSIGNAL` or `SO_NOSIGPIPE` only shows on the console.
- **Real memory of a Vita**: Vita3K runs on the Mac's memory, an out-of-memory on the console
  can pass here.
- **Hardware decoding**: Vita3K does not emulate the `h264_vita` decoder (`decoder not ready`
  in the app log), the picture comes from another path than on the console.
- **Network playback on Switch**: Eden's sockets fail ffmpeg's connection (`Bad file number`,
  `assert flags == 0` in Eden's log). API calls work, playback does not.
- **Stremio on Vita**: with a Stremio account, Vita3K freezes on the home screen while the app
  loads the addons over HTTPS, on every build tested. Jellyfin over HTTP works.
- **Torrent playback on Switch**: the player cannot open the torrent engine's local URL, with the
  same `Bad file number` as above, so peers, speed and playback cannot be measured.
- **App log on Switch**: the app writes its log to a file only with `-o <path>`, and Eden does
  not pass command-line arguments the way a homebrew `.nro` reads them.
- **Speed**: Eden runs single-core (its multi-core JIT crashes the app), the Switch build takes
  about 40 s to show its first screen.
