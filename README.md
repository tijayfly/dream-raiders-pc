# Dream Raiders – PC port

A drop-in `d3d9.dll` that runs SEGA's 2012 RingWide light-gun game **Dream Raiders** natively on a Windows PC.
You run the game's own `Game.exe` directly: no emulator, no launcher and no arcade hardware. The port stands in for
the cabinet hardware, fixes what breaks above 720p, and adds PC-friendly controls, display options and a pause menu.

**This repository contains no game data.** You need your own copy of the RingWide build of Dream Raiders
(`Game.exe` dated 1 Aug 2012).

## Enhancements

Compared with running the game through emulation:

- **Runs the original `Game.exe` directly.** Drop one DLL in and double-click; no loader, launcher or profile setup.
- **Native resolution, any size.** 2D and 3D render at your screen's resolution in 16:9, in a borderless or
  normal window of any size, letterboxed or stretched to fit.
- **Mouse/Keyboard and gamepad built in.** Mouse aiming plus XInput pads with adjustable aim speed and deadzone. By default, Player 1
  is assigned Mouse/Keyboard and pad 1, and can use either straight out of the box. Player 2 is assigned to pad 2 by default.
- **No calibration, ever.** Both guns are calibrated automatically every frame, so the arcade test menu is never needed.
- **Rumble instead of the motion seat.** Pads pulse as you fire, with adjustable strength.
- **Fast boot.** The arcade start-up and hardware-check screens are replaced by a "Loading Dream Raiders" screen,
  and the title appears in about 1.5 seconds instead of about 11.
- **Real pause.** Esc freezes the picture and the sound and opens an in-game menu.
- **Settings in-game.** Controls, display, rumble, difficulty, language, subtitles, continues, timers, attract
  sound and free play are all in the pause menu, so there's no operator test menu to dig through.
- **Free play by default.** No coin-up; press Start. Credit mode can be enabled in the menu, and coin keymaps are present.
- **Steady 60 fps on any monitor.** V-sync on 60 Hz screens, and a precise frame limiter on high-refresh screens.
- **Clean presentation.** The game's leftover keyboard debug keys are disabled, and cabinet gameplay options are accessible from the popup menu.

## Install

1. Copy `d3d9.dll` next to `Game.exe` (the one in the game's top-level folder, not `Prg\`).
2. Run `Game.exe` directly (not `Game.bat`, which runs the arcade hardware check first).
3. The first run creates a `port\` folder beside the exe, and a `Dream Raiders` shortcut that you can move anywhere.

To go back to another setup (e.g. TeknoParrot), rename or remove `d3d9.dll`. The two can't run from the same folder
at the same time.

## Controls

| | Mouse / keyboard | Gamepad (XInput) |
|---|---|---|
| Aim | mouse | right or left stick, R3 recentres |
| Fire | left button | RT or A |
| Start | Enter | Start |
| Coin (when free play is off) | 5 / 6 | View/Back |
| Pause menu | Esc | |

Player 1 defaults to mouse + pad 1 (whichever moved last aims); player 2 to pad 2.

## Settings

Everything is in the Esc menu and saved to `port\DreamRaidersPort.ini`:

| Section | Settings |
|---|---|
| `[Display]` | `Mode` (borderless / windowed), window and render size (0 = automatic), `Aspect` (letterbox / stretch), `VSync`, `FpsLimit`, `Monitor` |
| `[Input]` | `Player1` / `Player2` sources, `PadSpeed`, `PadDeadzone`, `InvertX` / `InvertY`, `ConfineMouse` |
| `[Rumble]` | `Enabled`, `Strength` |
| `[Game]` | `FreePlay`, `ResetSaveOnNextLaunch` |
| `[GameOptions]` | difficulty, language, subtitles, continues, continue and dream-select timers, attract sound (-1 leaves the game's own value) |

The `port\` folder also holds:
- `sram.bin` / `eeprom.bin`: the cabinet's backup memory (scores and settings)
- `Game.runtime.ini`: the game's ini with display overrides applied (your `Game.ini` is untouched)
- `port.log`: what the port did, including the game library's own messages. Check here first if something goes wrong.

## How it works

The DLL is loaded by the game in place of the system `d3d9.dll`. It forwards to the real one, and on load patches
`Game.exe` in memory. Each patch is checked against the expected original bytes first. If `Game.exe` isn't the
expected build, the port shows a warning and leaves the game unpatched.

- **Cabinet hardware emulation** (`src/patches.cpp`): lets the game boot on regular PC hardware. The patches also cover the boot speed-ups, dynamic resolution fixes, free play, automatic calibration and writing
  the game options into the game's settings store.
- **JVS I/O board** (`src/jvs.cpp`): an emulated I/O board behind the `\\.\mxjvs` driver, with 2 players,
  2 coin slots, 8 analog channels (the positional guns) and 8 outputs.
- **Backup memory and devices** (`src/devices.cpp`): SRAM is kept in `sram.bin`, and the SMBus EEPROM and DIP
  switches in `eeprom.bin`. This file also holds the window hooks, the keyboard hook and the DirectSound pause.
- **Input** (`src/input.cpp`): mouse and XInput, mapped onto the gun analog channels, plus rumble.
- **Display and pause** (`src/gfx.cpp`, `src/overlay.cpp`): device and present hooks, scaling, the frame limiter,
  the loading screen, the icon, and the Dear ImGui pause menu.

## Building

```sh
./build.sh
```

This cross-compiles with mingw-w64 (`i686-w64-mingw32-g++` and `windres`) and outputs `build/d3d9.dll`. On
Windows, run the same command from an MSYS2 MINGW32 shell. The version number is in `src/version.h`.

Dear ImGui 1.91.9 is vendored in `third_party/imgui` (MIT, see its `LICENSE.txt`). Only the files the build uses
are included.

## AI disclosure

This port was developed with substantial help from AI (Anthropic's Claude). The AI wrote most of the code, the icon and the documentation, working under the
project author's direction. The author set the goals and design decisions, tested every build on PC,
and reviewed and edited the results.

## Legal

This project contains no SEGA code or assets. It's an independent compatibility layer that runs a copy of the game
you already own. Don't redistribute game files with it.

The port's own code is released under the [MIT License](LICENSE). Dear ImGui keeps its own MIT licence, and
Dream Raiders and all game files remain the property of SEGA.