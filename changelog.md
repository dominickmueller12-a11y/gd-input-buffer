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
