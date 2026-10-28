# Input Buffer – Geode mod for Geometry Dash 2.2081

Click a few ticks early and the click is moved to the tick where it actually works.
Everything is configurable in game from the Geode mod settings page (and with hotkeys).

```
Settings for Input Buffer
├─ How to read the timing numbers      (info)
├─ Main Buffer Window                  Off / 2 / 4 / 8 / 12 / 16 / 24 / 32 ticks
├─ Landing and orb modes
│    Cube / Ball / Robot / Spider Window (ticks)     -1 = main, 0 = off
├─ Flying modes
│    Ship / UFO / Swing / Wave Window (ticks)        -1 = main, 0 = off
│    Wave Mode                        Off / Press Only / Grid (Press + Release)
├─ Hotkeys
│    Cycle Main Window F6 · TPS Up F7 · TPS Down F8 · Debug Logging F9 · Cycle Wave Mode F10
└─ General
     Game Speed (TPS) 240 · Look-ahead 48 · Max simulated ticks 1200
     Buffer releases · Safe Mode · Show status label · Debug Logging
```

## How it works

1. Your press (or release) is intercepted before the game sees it.
2. The level is saved with the game's own checkpoint system (`PlayLayer::createCheckpoint`).
3. The **real** physics (`GJBaseGameLayer::processCommands`) are run ahead with the input placed
   0, 1, 2 … N ticks later. Deaths, completions, sounds, stats and the wave trail are muted
   while this happens.
4. The earliest surviving placement is chosen (binary search, so even a 32-tick window only
   costs ~7 short simulations), the level is rewound, and the input is queued for that tick.
5. Every input that arrives while one is still waiting is shifted by the same amount, so taps
   stay taps and holds keep their length.

If the input is already safe, or nothing inside the window survives, it goes through unchanged.

## Building

### Easiest: let GitHub build it

1. Create a new GitHub repository and push this folder to it.
2. Open the **Actions** tab. The *Build Geode Mod* workflow runs on every push and builds the
   mod for Windows.
3. Download the **Build Output** artifact and drop the `.geode` file into
   `<Geometry Dash folder>/geode/mods/`.

### Locally (Windows)

Install the [Geode SDK and CLI](https://docs.geode-sdk.org/getting-started/), then:

```
geode sdk install
geode sdk install-binaries
cmake -B build -A x64
cmake --build build --config RelWithDebInfo
```

If the Geode CLI knows where your game is (`geode config setup`), the built `.geode` file is
copied into the mods folder automatically.

## Notes

- Requires Geode **5.11.2** or newer and GD **2.2081**.
- **Safe Mode** (default on) runs levels in test mode while the buffer is on, so completions
  and percentages are **not saved**. Turn it off if you want them saved; nothing made with
  this mod counts as a legitimate run anyway.
- **Game Speed (TPS)** above 240 works like a physics bypass. The physics feel changes, and
  it is the most experimental part of the mod.
- Not designed to run together with other input-timing mods (Click Between Frames, other
  input buffers, bots). Disable those while testing.
- Debug Logging (F9) writes one line per decision to `geode/logs/`.
