# Input Buffer

Clicked a few ticks too early? The click is moved to the tick where it actually works.

When you press (or, in flying modes, release), the mod saves the level with the game's own
checkpoint system and quietly runs the **real game physics** a short way ahead with your input
placed 0, 1, 2 … N ticks later. It takes the earliest placement where you are still alive,
rewinds, and plays your input for real on that tick. If your input was already fine, or you
were too early even for the window, it goes through unchanged – exactly like vanilla.

## Settings

- **Main Buffer Window** – the window for every gamemode. *Off* turns the mod off.
- **Per-gamemode windows** – `-1` uses the main window, `0` turns that mode off, anything
  else is a custom window in ticks.
- **Wave Mode** – *Off*, *Press Only*, or *Grid* (both presses and releases are moved, so both
  corners of a zig-zag fit the gap).
- **Hotkeys** – F6 cycles the main window, F7/F8 change TPS, F9 toggles debug logging,
  F10 cycles the wave mode. All rebindable.
- **Game Speed (TPS)** – physics ticks per second. 240 is vanilla.
- **Look-ahead** – how far ahead a death is looked for.
- **Safe Mode** – runs levels in test mode while the buffer is on, so nothing is saved as a
  real completion.

## Timing numbers

Geometry Dash runs physics at 240 ticks per second: **1 tick = 1 frame at 240 FPS, 4 ticks = 1
frame at 60 FPS**. A window of 8 ticks means a click up to 2 (60 FPS) frames early still works.

## Be honest

This is an assist. Runs made with it on are not legitimate and are not accepted on any list or
leaderboard. Keep Safe Mode on unless you know what you're doing.
