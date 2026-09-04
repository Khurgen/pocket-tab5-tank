# Progression design — locked decisions (2026-08-20)

Decisions made with Strato. This is the contract for the progression layer;
implementation lands across sim (Track 2 polish) and firmware (Tracks 3/4).

## Core principle

**The tank is shaped by attention, never ruined by absence.** Fish never die.
If left unattended, progression plateaus — nothing decays into guilt. A desk
companion, not a demanding pet.

## Persistence & stats UI

- Fish stats/traits/age/history persist in NVS flash (hundreds of bytes).
- **No ambient UI clutter.** Stats are revealed on demand only:
  - tap a fish → its stat card, **visual only** (bars/icons, no tiny digits), or
  - a global UI toggle to show/hide overlays.
- *Sim implementation (2026-08-20):* click a fish → selection ring in its body
  color + card (border = fish color as its identity, six bars with legend dots:
  hunger/energy/stress/curiosity/bold/social, stage pips 1–4). Click again or
  elsewhere to dismiss; `U` hides all overlays. Sleep visual: resting fish at
  night close their eyes. Same renderer code carries to the device; touch maps
  to the click path.
- Milestones live in a **separate view**, never the main tank.

## Real-time continuity (RTC)

*Implemented 2026-08-21 in `common/progression.c` (ravenous rule on boot when the
save is ≥1 h old; sim: file + wall clock; device: NVS + esp time, RTC hookup in
Track 4).*

Keep it simple — one rule:
- Off for **≥1 hour** → on boot the fish are **visibly ravenous**: they wait
  near the surface and chase the first pellets aggressively.
- After the **first feeding**, behavior returns to normal. No other offline
  simulation, no accumulated penalties.

## Growth (the Tamagotchi moment)

*Implemented 2026-08-21: stage from tended age (fry→juv 20 min, adult 90 min,
elder 8 h; lights-off pauses aging), size = stage scale × meal bonus; silent.*

- Well-fed fish visibly grow; colors richen with health; fins elaborate with
  age. All procedural sprite parameters.
- Stage changes should feel like **discovered surprises** — no announcements,
  the player notices. Stages: fry → juvenile → adult → elder.

## Hunger meter

- Hunger is a first-class pressure: it **mechanically scales food-pursuit
  aggression in the reflex layer** (speed and how little the fish brakes on
  approach), regardless of which brain chose seek_food. Implemented in
  `sim/tank.c` (2026-08-20); the ravenous-boot behavior falls out of it.
- UI: exposed as a simple visual meter only when the stats UI is revealed.
- *Economy retuned 2026-09-01* (`tank.c` HUNGER_*/TRICKLE_*): a meal lasts
  ~5-6 min awake, the tank's trickle is hunger-gated and per-second, so an
  untended awake tank hovers between fed and peckish and **ravenous begging
  is the after-sleep / long-absence event only**. `--selftest-hunger`.

## Trait drift

*Implemented 2026-08-21: bold drifts down under sustained stress, up when fed
and calm; social drifts up while following friends, down while solo exploring —
one trait unit per ~6 h of pressure. Trust (touch) also persists.*

- In, but **kept simple**: two drifting traits only — **bold** and **social**
  (0–9). Slow drift (days), small event-driven nudges, always recoverable.
- These traits enter the model's state string (schema v2) so the LLM expresses
  personality; this is the core LLM-over-rules advantage.

## Touch interactions (trust)

*Implemented 2026-08-21 in `common/tank.c` (tank_touch_hold/tap); sim mouse
and device FT3168 (`firmware/main/touch_port_ft3168.c`) feed the same state
machine.*

- **Tap-and-hold** (settled, ~3 s of contact — 2026-08-30): fish swim toward
  the held spot (trust-gated approach; a strongly hungry fish ignores the
  finger). The 3 s gate keeps taps/card-taps from twitching the school.
- **Aggressive taps**: fish flee the impact site.
  - While fleeing, continued taps (chasing) keep them fleeing until a
    **cooldown timer** resets.
  - After cooldown, it takes **3 consecutive quick taps** to trigger flee
    mode again (single taps become harmless).
- Double-tap reserved for the light toggle (see below).
- Tap reactions are reflex-layer, not model decisions.

## Upkeep chores (2026-08-30, `tank_veg_bed`/`tank_grow_algae` in tank.c)

- **Vegetation is alive**: the three beds keep growing — up toward the
  surface (fastest during device drowse). **Every frond has its own height**
  (2026-09-04, `tank_t.veg_h`): a sideways stroke that starts on a bed cuts
  exactly the fronds it crosses, at the height where the finger crosses
  them — a flick takes one or two at that height, a sweep along the floor
  mows the bed toward **nubs, never bare** (little bits of green always
  remain). Fish swim slower inside a canopy. **Height is
  growth** (2026-09-04): a bed at growth g stands g of the way from the
  floor to the surface — only the tank ceiling limits it, every bed alike.
  Comfort band (2026-09-04 rework — fish LIKE cover): any canopy calms
  (stress decays faster with more grass, faster again for a fish tucked
  inside one, and a hidden fish feels a shadow at half the press); a fully
  scalped tank (no bed past `VEG_BARE`) is a **mild** unease that lifts the
  moment one tuft regrows; only a tank being **smothered** presses back —
  the second-tallest bed past `VEG_SMOTHER` (85% of the way to the surface,
  i.e. at least two beds crowding the ceiling), ramping to the full press
  at 100%. One bed at the ceiling is just a good place to hide. Stress is
  already in the schema, so the model reacts without any schema change.
- **Algae films the glass** over hours (2x during drowse), dappled from the
  corners/edges in; a **drag across the glass squeegees it clean**. Film
  steps run through `tank_t.algae_acc` awake AND asleep — the device
  drowses in 30 s slices and the old per-call `(int)(30 / 120)` had been
  truncating every overnight step to zero (fixed 2026-09-04).
- Tank milestones: first trimming, first glass cleaning.
- Neither chore ever counts toward the tap burst (no accidental startles).

## IMU (motion)

- **All-or-nothing**: only ship if it can be dialed in — tilting the tank must
  redirect bubbles and adapt physics convincingly. If the board can't do it
  well, skip the feature entirely. Prototype in firmware before committing.

## Light discipline / sleep

- Light off → fish retreat to the **seaweed/reef corner** and enter visible
  sleep mode. Obvious, readable behavior.
- Doubles as a **stasis/pause mode**: light off ≈ pausing the tank without
  powering down. (Progression effectively pauses while asleep.)

## Breeding

*Not implemented yet — one design implication surfaced 2026-08-21: the model's
vocabulary is closed (58 tokens) and fish are identified by name in the state
line, so a fry needs a name token the model was trained on. Options: (a) add a
5th name to the lexicon now and include 5-fish scenes in the next data cycle
(cheap: regen + retrain), or (b) keep the fry reflex-only (follows a parent,
no advisor) until it matures into a vacated/known name. Decide before breeding
is built.*

- **Conservative and special.** Two well-fed, mature fish can breed — a rare
  "I didn't know they could do that" moment, not an economy.
- Population: 4 fish + **at most 1 fry** until that fry matures. Hard cap.
- Scripted/reflex-orchestrated event, not a model decision (no new goal enum).

## Model impact (schema v2 — pending approval)

Only three additions to the state line; everything else above is reflex-layer:

```
bold <0-9> social <0-9> stage <fry|juv|adult|elder>
```

- Teacher prompt v2 describes how personality and life stage shade decisions;
  traits randomized during trace generation so the student learns the whole
  personality space.
- Ravenous-boot needs no schema change (it's hunger 9 + reflex urgency).
- Sleep needs no schema change (`time night` already exists; teacher prompt
  gains "fish rest by the reef at night").
- Cost: ~+6 tokens per state line; one overnight regen + one retrain.
