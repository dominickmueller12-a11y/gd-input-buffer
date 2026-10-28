# v2.1.0
- Fixed the reason v2.0.1 still did nothing: the "the game spent your press" detector compared y-velocity between frames against a threshold of 2.0, but the check runs once per frame and that is four physics ticks, so ordinary falling moves y-velocity about 3.5 and tripped it instantly. Every press was marked spent on its first frame and the release went through untouched - 26 presses held, 0 ever buffered.
- It now uses a sign test instead: gravity can only pull you the way gravity points, so the first tick you move the other way is the tick the press was spent on. No threshold to get wrong. A gravity flip (ball, spider, blue orb) counts as spent too.
- A press arriving while a release is still held back no longer injects a second press. That double-send is what used to read as three clicks per click.
- Simple mode now covers ship, UFO, swing and wave as well, as a minimum tap length rather than a wait: a tap shorter than "Minimum tap length" is stretched to it, so fast inputs come out the same every time. Default 10 ticks.

# v2.0.1
- Simple mode now leaves ship, UFO, swing and wave completely alone. Holding the button in those modes is the control itself: a ship accelerates gradually, so there is no single tick where the press is spent to let go on, and the button would have stayed down far too long. A click in a flying mode is never thrown away in the first place, so there is nothing there to buffer.

# v2.0.0
- New Buffer Mode setting, defaulting to Simple, which does not predict anything. The click goes through instantly and the button is held a few more ticks so the game spends it on the next landing or orb. Geometry Dash already jumps the moment you land while holding, so the game decides the timing and the whole class of bug that broke v1.5, v1.6 and v1.7 cannot happen. Orbs work for free, because holding through an orb activates it.
- The held press is let go on the tick the game spends it, spotted by y-velocity moving further in one tick than gravity ever does. That works in every mode, gravity and speed without asking a flag anything.
- Predictive mode is still there as an option, and no longer copies m_touchedRing, m_touchedPad or m_touchedGravityPortal into the preview player. m_touchedPad read true on every single press of a real session, so the preview thought it was on a jump pad, decided every press already did something, and passed all of them through unbuffered.

# v1.7.0
- The "does this press do anything" test no longer trusts m_isOnGround, m_touchedRing or m_touchedPad. Those read true while the player is falling, so every single cube press answered "works now" after zero simulated ticks and went straight through - v1.6.0 changed nothing for that reason.
- Instead the mod runs the ghost once with no input at all, then asks whether pressing on a given tick moves it off that path. A press the game throws away leaves the path untouched, which is the thing worth detecting, and it cannot be fooled by a stale flag.
- The first tick that does something is found by binary search, with a sweep afterwards for orbs and pads, which are only live for a few ticks.
- The decision log now records eff, the raw ground/ring/pad flags and the baseline length.

# v1.6.0
- The real fix for early clicks in cube, ball, robot and spider: a press in mid-air is not dangerous, it is thrown away by the game, so the old "does pressing now kill me?" test answered "no" and fired it into the void. The mod now asks when the press would actually do something (land, orb, pad) and holds it to that tick.
- The safety check for a held press uses a short horizon instead of the full look-ahead. Requiring survival for a whole second with no further clicks is something real gameplay never does, and it was the reason 59% of clicks fell back to vanilla with "nothing survives".
- Log reasons are now "works now", "held to landing", "landing unsafe" and "no landing in window".

# v1.3.0
- Presses are no longer ignored (the game tags presses as player 2 on some setups; all inputs are handled now).
- A jump from the ground is buffered too: clicking too early before a spike now delays the jump until it clears, which is the whole point.
- Duplicate deliveries of the same input no longer cancel a delay.

# v1.2.1
- Fixed: the preview ghost never moved forward (2.2 moves the player in updateMove, not update), so it could never reach the hazard. This is the one that makes the buffer actually work.
- A portal you are already inside no longer ends the preview early.
- Each click is evaluated once instead of twice.

# v1.2.0
- Fixed: early clicks in cube/ball/spider were never shifted (the preview wrongly counted "landed on the ground" as safe).
- Bigger windows: presets up to 120 ticks (half a second); per-mode windows up to 120. The look-ahead automatically extends past the window.
- Ghost copies position and velocity explicitly; debug line shows ghost vs. real position.

# v1.1.0
- New preview engine: a hidden ghost player is simulated instead of stepping and rewinding the whole level. Triggers, moving objects, the wave trail, music and other mods are no longer affected by the preview. Fixes fast-forwarding lava/clouds, 3 CPS per click and the disappearing wave trail.
- New setting: "Preview uses orbs, pads and gravity portals".

# v1.0.3
- Flying modes: the preview now assumes a short hold after a press and a re-press after a release (new "Assumed hold length" setting). Releases in wave/ship are no longer delayed just because "never pressing again" would hit the floor.

# v1.0.2
- Hooks now run after other mods: preview inputs no longer inflate CPS counters or trip speedhack detectors.
- Music is no longer re-seeked during the preview rewind (this was the big, random stutter).

# v1.0.1
- Grounded presses in cube/ball/robot/spider skip the simulation (no stutter on normal jumps).
- Simulation time is taken out of the next frame instead of the game catching up with a jump.
- Default look-ahead lowered to 32 ticks.
- Debug log now shows where the time goes (checkpoint / rewinds / per tick).

# v1.0.0
- First release.
