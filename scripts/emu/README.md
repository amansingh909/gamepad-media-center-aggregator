# Console emulation harness

`ctl.py` runs the Switch (`.nro`) and PS Vita (`.vpk`) artifacts of a CI run in an emulator
on macOS arm64, so an agent can launch the app, navigate with a gamepad, capture the screen,
read the logs and catch a crash or a freeze without a console.

| Platform | Emulator | Input | App log |
|---|---|---|---|
| `nx` | [Eden](https://git.eden-emu.dev/eden-emu/eden) v0.2.1, pinned by SHA-256 | DSU (cemuhook) server played by `ctl.py` | not available, `log` shows Eden's |
| `psvita` | [Vita3K](https://github.com/Vita3K/Vita3K) `continuous` | virtual SDL gamepad loaded into Vita3K (`vpad.c`) | the app lines of `vita3k.log` |

The emulator window stays in the background and never takes the focus: Eden runs as an agent
app (`LSUIElement`), Vita3K with SDL hints. Captures use `screencapture -l <window id>`.

## Setup

Requirements: `gh` signed in, `brew install sdl3` (headers for `vpad.c`), and the
"Screen Recording" permission for the terminal.

```bash
scripts/emu/ctl.py setup
```

Everything lands in `~/.cache/gmca-emu` (`EMU_HOME`): Eden in portable mode (`eden/user`),
Vita3K in portable mode (`vita3k/portable`) with Sony's 3.74 firmware, downloaded from
playstation.com and checked by SHA-256. Neither emulator needs keys: Eden synthesizes the
Switch shared fonts, the Vita app ships its own.

Ryubing was the first choice for Switch. Its servers (ryujinx.app, git.ryujinx.app) have been
unreachable since September 2026.

## Use

```bash
scripts/emu/ctl.py fetch 40083100934                  # artifacts of a CI run
scripts/emu/ctl.py launch nx 40083100934 --fresh      # or a path to a .nro/.vpk
scripts/emu/ctl.py key a                              # up/down/left/right/a/b/x/y/l/r/zl/zr/plus/minus
scripts/emu/ctl.py shot home                          # ~/.cache/gmca-emu/shots/home.png
scripts/emu/ctl.py log 20
scripts/emu/ctl.py check                              # crash, then freeze over FREEZE_SECS
scripts/emu/ctl.py quit

scripts/emu/ctl.py run scripts/emu/scenarios/smoke.txt psvita 40083100934
```

`fetch` refuses a run built from a fork, whose code would run on this Mac and whose log would
reach the agent reading it; `EMU_UNTRUSTED=1` accepts it.

A CI run without a PR: `gh workflow run build.yaml -R thcolin/gamepad-media-center-aggregator
--ref <branch> -f platform=nx` (or `psvita`), one run per platform.

Buttons follow the Switch layout borealis uses: `a` confirms, `b` goes back, `y` opens the
settings. On Vita, `a` is Cross, `b` Circle, `x` Square, `y` Triangle.

`launch --fresh` wipes the app data and writes a config without the one-time prompts (update,
HOME tile). `launch --jellyfin` also signs in to the test Jellyfin of
`plenx-media-servers/docker-compose.yml` with the `gmca-emu` account, whose password is in the
keychain item `gmca-emu-jellyfin`: neither emulator offers an on-screen keyboard usable without
the window focus, so the sign-in screen itself is not exercised.

## Scenarios

- `smoke.txt`: fresh config, Connections, connection types, settings. No network.
- `jellyfin.txt`: signed in, home, the Movies library, the movie, ten seconds of playback with `check`, back. On
  `nx` the playback shows Eden's socket error, see below.

`test_ctl.py` checks the DSU packet against what Eden validates.

A scenario stops at the first crash and ends on `VERDICT : OK` or `VERDICT : ÉCHEC`. `check`
catches a crash or a frozen screen; an error dialog is neither, the captures are there to be
read.

## What an emulator does not catch

- **Socket constants of OpenOrbis (PS4)**: PS4 is out of scope, and a wrong `MSG_NOSIGNAL` or
  `SO_NOSIGPIPE` only shows on the console.
- **Real memory of a Vita**: Vita3K runs on the Mac's memory, an out-of-memory on the console
  can pass here.
- **Hardware decoding**: Vita3K does not emulate the `h264_vita` decoder (`decoder not ready`
  in the app log), the picture comes from another path than on the console.
- **Network playback on Switch**: Eden's BSD sockets fail ffmpeg's connection (`Bad file
  number`, `assert flags == 0` in Eden's `network.cpp`). API calls work, playback does not.
- **App log on Switch**: Eden passes `program_args` the NSO way, libnx reads argv from the
  homebrew loader, so `-o` never reaches the app.
- **Speed**: Eden runs single-core (its multi-core JIT crashed the app), the Switch build takes
  about 40 s to show its first screen.
