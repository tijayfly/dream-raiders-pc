# Changelog

## 1.0.0 – 2026-11-17

First release.

- Runs the RingWide `Game.exe` (1 Aug 2012 build) directly on Windows through a drop-in `d3d9.dll`.
- Emulates the JVS I/O board, SRAM, EEPROM, DIP switches and other cabinet hardware as needed for the game to run properly.
- Renders 2D and 3D at native resolution in 16:9, in a borderless or normal window of any size, letterboxed or stretched.
- Fixes the title logo scaling and the result-screen rank clipping that broke above 720p.
- Mouse and XInput gamepad aiming, with aim speed, deadzone and per-player source settings.
- Automatic gun calibration, so the arcade test menu is never needed.
- Rumble in place of the motion seat.
- "Loading Dream Raiders" screen and fast boot: about 1.5 s to the title instead of about 11 s.
- Esc pause menu that freezes the picture and sound, with Pause Menu, Controls, Display, Rumble and Game tabs.
- Game options (difficulty, language, subtitles, continues, timers, attract sound) set from the menu.
- Free play on by default.
- 60 fps with v-sync on 60 Hz screens and a precise limiter on high-refresh screens.
- The game's keyboard debug keys are disabled.
- Dream Raiders window icon, and a shortcut created on first run only.
