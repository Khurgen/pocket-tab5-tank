# Pocket Tab5 Tank v0.1.0 Physical Acceptance Checklist

Build: UIFlow Edition v0.1.0 Living Aquarium

## Startup

- [ ] UIFlow2 connects to Tab5.
- [ ] `/flash/pocket_tab5_tank/app_engine.py` is present.
- [ ] Run Once starts without a syntax error.
- [ ] WebTerminal shows `setup start`.
- [ ] WebTerminal shows `living aquarium ready`.

## Display

- [ ] Screen is landscape 1280 x 720.
- [ ] Blue aquarium fills the screen.
- [ ] Sand floor is visible.
- [ ] Left and right plant beds are visible.
- [ ] Central rocks are visible.
- [ ] Bubble column is visible.

## Animation

- [ ] Orange fish moves continuously.
- [ ] Teal fish moves continuously.
- [ ] Fish travel at visibly different speeds.
- [ ] Fish follow different vertical trajectories.
- [ ] Fish reverse direction when reaching tank edges.
- [ ] Fish eye/tail positions change correctly when direction reverses.
- [ ] Bubbles move upward and reappear near the bottom.

## Stability

Start time: __________________

- [ ] 2 minutes, stable
- [ ] 5 minutes, stable
- [ ] 10 minutes, stable
- [ ] No Python exception
- [ ] No spontaneous reboot
- [ ] No frozen fish
- [ ] No persistent screen corruption

## WebTerminal

Copy the final three `aquarium alive` lines here if a problem occurs:

```

```

Observed issue, if any:

```

```

Result:

- [ ] PASS, freeze v0.1.0
- [ ] FAIL, diagnose before adding features
