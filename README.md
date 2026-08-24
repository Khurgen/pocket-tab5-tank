# pocket-tank 🐟

**A tiny language model keeps a fish tank alive on an $8 chip.**

A 14-million-parameter transformer — distilled from a 26-billion-parameter
teacher — runs entirely on an ESP32-S3 microcontroller and makes every
high-level decision for a small aquarium of virtual fish: when to eat, hide,
explore, socialize, rest, or flee. No network. No cloud. The whole brain is a
7.56 MB file read straight from flash.

![The tank, running in the PC simulator](docs/media/sim-tank.png)

The fish aren't scripted. Each one periodically describes its situation to the
model as a single line of text — hunger, energy, fear, who's nearby, its own
personality — and the model completes the line with a goal and an urgency. A
60 fps reflex layer turns that goal into motion. When the model isn't sure,
the fish visibly *hesitates*.

```
fish mira zone 2 hunger 7 energy 5 stress 2 curiosity 8 bold 4 social 6
stage adult food near 12 shadow far 6 friend bolt mid 3 bubble mid 10
reef far 7 wall clear last explore time day  ->  seek_food urgency 8
```

## The numbers

| | Teacher | Student (what ships) |
|---|---|---|
| Model | gemma4:26b | pocket-tank 14.3M (dim 384, 8 layers, 8 heads) |
| Parameters | ~26,000,000,000 | 14,300,000 — ≈1,818× fewer |
| Size | ~18 GB (Q4_K_M) | 57 MB fp32 → **7.56 MB 4-bit** |
| Vocabulary | ~262K tokens | **58 tokens** (a closed schema lexicon) |
| Runs on | A desktop GPU | ESP32-S3, from flash, no network |
| Agreement | — | 75% picks the teacher's goal (measured ceiling ≈82%) |

Trained on 51,162 teacher-labeled situations. Full measured numbers, learning
curves, and methodology: [docs/stats.md](docs/stats.md).

## How it's put together

Three layers, strictly separated:

- **Reflex layer** (`common/tank.c`) — 60 fps physics, steering, schooling,
  touch gestures, feeding. Runs everywhere, never blocks.
- **LLM advisor** (`common/llm/`) — the 4-bit inference engine
  (llama2.c-style), a word-level tokenizer, and one shared
  encoder+inference core used verbatim by both the simulator and the
  firmware. Fish are re-asked only when their situation meaningfully changes.
- **Progression** (`common/progression.c`) — the long game: a tank starts
  with two fish and *earns* up to six through care; fish grow through life
  stages, drift in personality under sustained pressure, hit milestones
  (first meal from you, surviving a shadow, a follow…), and form habits like
  remembering where you feed them. All persisted; power loss safe.

One deliberate rule: **the model owns its decisions.** There are no fallback
heuristics second-guessing it. If the model picks a goal, the fish commits —
uncertainty is expressed as visible hesitation, not silently overridden.

## Try it — PC simulator

Needs SDL2 and LVGL v9 (cloned in-tree):

```bash
git clone https://github.com/mediacutlet/pocket-tank.git
cd pocket-tank
git clone --depth 1 --branch v9.2.2 https://github.com/lvgl/lvgl.git sim/lvgl
brew install sdl2        # macOS; apt install libsdl2-dev on Linux
cd sim && make && ./fishsim
```

The trained model (`model/out/model_q4.bin` + `tokenizer.bin`) ships in the
repo, so the LLM brain works out of the box. In the window: tap the water
surface to feed, click a fish for its stats card, hold the mouse as a finger
on the glass, **S** casts a shadow, **L** toggles rule-stub vs LLM brain,
**M** shows milestones, `--narrate` prints every decision as it's made.

## Try it — firmware (no hardware needed)

The ESP-IDF app for the Waveshare ESP32-S3-Touch-AMOLED-1.8 boots in
Espressif's QEMU with the model partition populated:

```bash
cd firmware && ./run_qemu.sh
```

You'll watch an emulated ESP32-S3 load the 7.56 MB model from flash and start
making decisions (~2.3 s each in emulation). `docs/bringup.md` is the
real-hardware checklist.

## Train your own

The whole distillation pipeline is here: `model/gen_traces.py` runs the
headless tank against any Ollama teacher and logs state→goal pairs;
`train_tokenizer.py`, `train.py`, and `export_q4.py` take it from JSONL to a
flashable 4-bit binary. It builds on a clone of
[karpathy/llama2.c](https://github.com/karpathy/llama2.c) in
`model/llama2.c/`. Step-by-step commands: [docs/pipeline.md](docs/pipeline.md).

## Layout

- `common/` — everything shared verbatim by sim and firmware: reflex layer,
  renderer, progression, and the LLM engine
- `sim/` — LVGL + SDL PC simulator
- `firmware/` — ESP-IDF v5.4 app + QEMU harness
- `model/` — schema, trace generation, training, evaluation, 4-bit export
- `docs/` — the [state/goal schema](model/schema.md) is in `model/`;
  docs has [measured stats](docs/stats.md), the
  [progression contract](docs/progression.md) and
  [its design](docs/progression-next.md), the
  [memory budget](docs/memory_budget.md), and the
  [hardware bring-up checklist](docs/bringup.md)

## Status

- ✅ Model: schema v3, 14.3M student, 4-bit export, evaluated
- ✅ Simulator: full tank with progression, self-tests, snapshots
- ✅ Firmware: builds, QEMU-verified end-to-end with the LLM advisor
- 🚧 Hardware bring-up on the real AMOLED board — this is what the video
  series is following

## The video series

This repo is the companion to a YouTube series documenting the build —
distilling the model, designing the schema, squeezing inference into PSRAM,
and (soon) first boot on real glass.

**▶ Watch:** _link coming with the first episode_

## License

MIT — see [LICENSE](LICENSE). `model/llama2.c/` and `sim/lvgl/` are cloned
separately and carry their own MIT licenses.
