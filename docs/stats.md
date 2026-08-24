# Measured numbers

Every figure below was measured in this repo's pipeline; dates are when. "Teacher" = gemma4:26b (Q4_K_M) via Ollama; "agreement" = the student
picked the same goal as the teacher on fresh, never-trained-on situations
(`model/eval.py --teacher`, n = 60 states, seed 777 unless noted).

## Teacher and student

| | Teacher | Student (ships) |
|---|---|---|
| Model | gemma4:26b | pocket-tank 14.3M (dim 384, 8 layers, 8 heads) |
| Parameters | ~26,000,000,000 | 14,300,000 (≈1,818× fewer) |
| Size | ~18 GB (Q4_K_M) | 57 MB fp32 → 15.2 MB int8 → **7.56 MB 4-bit** (ships) |
| Runs on | Mac Mini M4 Pro GPU | ESP32-S3 ($8–10 chip), from flash, no network |
| Vocabulary | ~262K tokens | 58 tokens (closed schema lexicon) |

## v1 learning curve (schema v1, no personality fields) — 2026-08-19/20

| Training pairs | Model | Agreement with teacher |
|---|---|---|
| 2,661 | 0.9M (dim 128) | 46% |
| 6,403 | 0.9M (dim 128) | 68% |
| 25,903 | 0.9M (dim 128) | 53% (capacity-saturated) |
| 25,903 | **14.3M (dim 384)** | **87%** (52/60) |
| 25,903 | 8.8M (dim 320) | 78% |

Same 60 states for the last three rows. Training a run takes ~75 s (tiny model)
to ~30 min (14M) on a Mac (Apple-silicon MPS).

## v2 personality model (schema v2: bold / social / stage) — 2026-08-21

| Measure | Value |
|---|---|
| Training pairs (clean, rebalanced prompt) | 25,626 trained on; 28,312 collected |
| Agreement with teacher (n=60) | 75% (45/60), 60/60 valid outputs |
| **Teacher agreement with itself** (same 40 states asked twice) | **82%** ← practical ceiling |
| Starving fish (hunger 9, food present, no predator) chooses seek_food | 94% (47/50) — misses: timid+stressed fish with a shadow in view fled; a very social fish followed a friend; a fry stayed with friends |
| Personality matrix vs teacher (bold 9 / bold 0 / social 9 / elder / fry) | 4/5 identical |
| Survival-reflex override | retired — the model owns starvation decisions |

## Tokenizer / latency — 2026-08-21

| | Char-level (v1) | Word-level (ships) |
|---|---|---|
| Vocab | 355 | 58 |
| Tokens per decision (state + goal) | ~200 | 46 |
| Per-decision latency, Mac, single thread | ~1,150 ms | **310 ms** (3.7× faster) |
| Device inference engine (host test) | — | 83 ms / decision, identical decisions to the reference runner on 30/30 |

## 4-bit quantization — 2026-08-21

| | fp32 | 4-bit (GS 64, fp16 scales) |
|---|---|---|
| File | 57 MB | **7.56 MB** |
| Agreement with fp32 decisions (60 fresh states) | — | 97% goals (58/60), 57/60 exact incl. urgency |

## The prompt-is-DNA incident — 2026-08-20

One sentence in the teacher prompt ("a social fish follows friends and dislikes
being alone") → **45%** of all labels were follow_friend (2,592 decisions).
Reworded ("friends are always nearby in a small tank — mere proximity is never a
reason") → **14%** (2,549 decisions). Full before/after distributions:

| goal | before | after |
|---|---|---|
| follow_friend | 45% | 14% |
| seek_food | 20% | 27% |
| flee_shadow | 15% | 18% |
| inspect_reef | 8% | 15% |
| visit_bubbles | 6% | 10% |
| explore | 3% | 9% |
| rest | 2% | 4% |
| dart_play | 1% | 3% |

## Data generation

~36–51 teacher decisions/minute with 2–3 workers (≈1.2–1.7 s per decision on the
Mac Mini); ~19,500 pairs in an 8-hour overnight run; 1,678 in a 55-minute burst.

## First run on the ESP32-S3 (QEMU, real hardware config) — 2026-08-21

Espressif QEMU 9.2.2, `esp32s3` machine, octal 8 MB PSRAM emulated, model read
from the mmap'd flash partition, scalar C, single core, no SIMD, no batching:

| | |
|---|---|
| Decision latency, token-by-token | ~2.85 s (46 tokens) |
| Decision latency, **batched prefill** | **~2.3–2.5 s** |
| Throughput | 15.8 → **~19 tok/s** |
| Render loop (core 0, stub display) | ~28 fps |
| PSRAM used | 3.3 MB of 8 MB (plan asserted OK) |
| App binary | 247 KB (no LVGL yet) |

QEMU is not cycle-accurate; treat as a first-order number until the board arrives.
Batched prefill (weights read once per layer for all prompt tokens) is in;
QEMU cannot model the flash-bandwidth win it targets, so expect a larger gap on
hardware. Remaining levers: ESP-DSP SIMD dot products, dual-core matmul split.

## QEMU 10-minute soak — 2026-08-21

196 advisor decisions in 10 minutes, latency steady at ~2.2 s (~20 tok/s),
0 errors/panics. Heap flat after the first minute: internal 350 KB free,
PSRAM 691 KB free (of QEMU's 4 MB; the batch-prefill buffers allocate once on
first use). Starvation-ignored instrument: 31 episodes in 10 min at QEMU's
decision latency (4 fish share one ~2.2 s advisor, so a starving fish can wait
~9 s for its turn) — expected to drop with on-hardware speedups; it is a
diagnostic, never an override.


## The student's goal distribution (what argmax threw away) — 2026-08-21

`model/probe_dist.py`, shipped v2 checkpoint, 800 real dataset states + probes:

| measure | value |
|---|---|
| teacher label == student top-1 | 79% |
| teacher label in student **top-2** | **94%** |
| "torn" states (top-2 margin < 0.2) | 14% |
| mean top-1 probability | 0.80 |
| P(sampled goal ≠ greedy) at T = 1 | 20% |
| starving (hunger 9, food near): min P(seek_food) over 18 identities | **0.89** |
| `friend none` (17 of 28,312 training pairs): starving lone fish | **flee_shadow 0.97** (no shadow) — a 1-fish tank is out |
| social 6 → 9 (content fish) | follow_friend 0.04 → **0.85** |
| bold 7 → 9 (content fish) | dart_play 0.0 → **0.25**, bubbles overtake reef |
| entropy fry / juv / adult / elder | 1.50 / 1.39 / 1.10 / 1.17 nats |
| shadow near 12 at stress 1 / 5 / 8 | P(flee) 0.19 / 0.47 / 0.82 (student learned stress as the cue; v3 fixes the data) |
| names mira/bolt/kelp/nori | identical distributions (zero signal) |

These numbers are the case for sampling on the device (the model's own
variety, survival untouched) and for trait drift as visible "unlocks".
Progression II build (same day): `./fishsim --selftest-llm`, 4 fish, 60 sim-s:
54 goal changes, 19 torn (hesitation shown), 113 need-based asks, 2
starving-ignored episodes.

Greedy vs sampled A/B (same harness, same seed): greedy 50 goal changes / 4
starving-ignored episodes; sampled 50 / 3 — sampling the model's own
distribution costs nothing on survival while restoring its variety.

## QEMU boot with Progression II — 2026-08-21 (stub overlay, 4 MB PSRAM)

Population 2 (pip + mira, cap 5), sampled decisions with p logged, ~2.2–2.5 s
per decision at 17.7–20 tok/s, need-based asks, heap flat (internal 348 KB,
PSRAM 363 KB free after the 330 KB scene cache), 0 panics in 160 s. 11
starving-ignored episodes in 160 s with 2 fish at QEMU latency (instrument,
not override; trickle-feed rate was retuned right after: `live < 2`, p =
0.001 × n_fish).


## Schema v3 data cycle — 2026-08-22

Overnight generation (gemma4:26b, 3 workers, 8 h cap): **22,850 clean v3 pairs**
(0 malformed, 0 off-vocabulary), ~48 decisions/min. `friend none` 604 pairs,
shadow-near-while-calm 568.

**Pure v3 student (22.8K pairs) — NOT shipped.** The v3 teacher prompt flattened
personality in the teacher's own labels (P(follow | social ≥ 8): 0.50 in v2 data →
0.18 in v3; content fish → visit_bubbles 0.20 → 0.47), and the student learned that:
social 0→9 moved follow_friend 0.00→0.03 (v2: 0.01→0.73), bold 0→9 moved dart_play
0.01→0.03 (v2: 0.00→0.16). Prompt-is-DNA, round two. (It did fix starving → 0.99 and
`friend none`.)

**v3m — SHIPS (sim + firmware, 2026-08-22):** the 28,312 v2 pairs converted
mechanically to the v3 line (names dropped, trust 5) + the 22,850 v3 pairs =
**51,162 pairs**, 14.3M student, 5,000 iters, best val loss 0.796, 4-bit 7.56 MB,
vocab 54.

| measure | v2 (previous ship) | v3 pure | **v3m** |
|---|---|---|---|
| teacher agreement (n=60, seed 777) | 75% | — | **75%** (ceiling 82%) |
| teacher label in student top-2 (own data) | 94% | 97% | 93% |
| social 0→9: P(follow), mean over 6 content states | 0.01→0.73 | 0.00→0.03 | **0.01→0.52** |
| bold 0→9: P(dart), same | 0.00→0.16 | 0.01→0.03 | **0.02→0.40** |
| shadow near 12, stress 1 / 8 → P(flee) | 0.19 / 0.82 | 0.22 / 0.34 | **0.56 / 0.86** |
| starving + `friend none` | flee_shadow 0.97 | seek_food 0.52 | **seek_food 0.98** |
| starving, min P(seek_food) over 18 identities | 0.89 | 0.99 | 0.82 |
| elder at night → rest | 0.97 | 0.99 | 0.98 |

Residuals: a lone fish (`friend none`) with a shadow near still prefers bubbles
(the v3 "lonely" cases had no shadows); shadow at *mid* range at low stress reads
as explore (as in v2; the tank's stress ramp masks it).

**prompt_check.py (new):** 340 teacher calls on a diagnostic panel in ~7 min.
Current v3 prompt vs a v3.1 candidate (v2 prompt + minimal trust): follow | social 9
0.15 vs 0.30; flee | shadow near calm 0.70 vs 0.35; content → bubbles 0.50 vs 0.50.
Neither prompt is clean; the bubble attractor is the teacher's own taste on calm
states, and v2's social cliff came as much from the "social" perturbation mix as
from wording. Conclusion: no v3.1 regeneration; v3m's cliffs come from the v2 labels.
