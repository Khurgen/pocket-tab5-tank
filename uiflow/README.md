# Pocket Tab5 Tank, UIFlow Edition

## v0.1.0 Living Aquarium

This is the first UIFlow2/MicroPython build of the Pocket Tank-inspired M5Stack Tab5 project.

### Scope

The v0.1.0 milestone intentionally proves only the living aquarium:

- 1280 x 720 landscape layout
- underwater scene drawn entirely with LVGL/M5UI objects
- two independently moving fish
- independent horizontal and vertical movement
- boundary avoidance
- visible direction changes
- animated bubble column
- lightweight serial health logging

Not included yet:

- touch interaction
- feeding
- fish needs or personalities
- behavioral decision engine
- persistence
- growth
- milestones
- sound
- RTC / IMU integration
- network access

## Device layout

Upload the engine to:

```
/flash/pocket_tab5_tank/app_engine.py
```

The repository file `run_once.py` is a tiny import launcher intended to be pasted into the UIFlow2 code editor for development testing.

## First physical test

1. Connect the Tab5 to UIFlow2 over USB.
2. Open WebTerminal.
3. Open Device File Manager.
4. Create `/flash/pocket_tab5_tank/` if it does not already exist.
5. Upload `app_engine.py` into that directory.
6. Paste the contents of `run_once.py` into the UIFlow2 code editor.
7. Select **Run Once**.
8. Keep WebTerminal visible.

Expected screen:

- blue aquarium
- sand floor
- plants at left and right
- central rock formation
- bubble column
- orange fish and teal fish moving independently

Expected terminal markers:

```
Pocket Tab5 Tank v0.1.0: setup start
Pocket Tab5 Tank v0.1.0: living aquarium ready
Acceptance test: leave running for at least 10 minutes
```

Approximately every five seconds, the program prints an `aquarium alive` line with the update rate and fish coordinates.

## Acceptance criteria

The build becomes the frozen v0.1.0 baseline only after all of the following pass on the physical Tab5:

- aquarium appears in correct landscape orientation
- both fish move
- both fish reverse cleanly at tank boundaries
- fish trajectories remain independent
- bubbles rise and recycle
- no visible flashing or full-screen redraw artifacts
- WebTerminal continues printing health lines
- no Python exception, reboot, or frozen display for at least ten minutes

Use **Run Once** for this test. Do not install as Run Always until the build passes.
