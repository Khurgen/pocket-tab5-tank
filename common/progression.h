/* progression.h — the "shaped by attention, never ruined by absence" layer
 * (docs/progression.md + docs/progression-next.md). Platform-agnostic;
 * persistence and wall-clock come in through two tiny port functions.
 *
 *  - Population: a new tank is two contrasting adults; the 3rd..POP_CAP-th
 *    fish ARRIVE as fry when care milestones are met (trust, feedings, a
 *    calm hold, a raised fry, drift). An arrival is staged when earned and
 *    shown at the next light-on (or boot) - an unannounced surprise.
 *  - Growth: well-fed fish grow (size) and advance fry -> juv -> adult -> elder
 *    on tended age; stage changes are silent surprises.
 *  - Trait drift: bold and social creep with experience (hours of pressure).
 *  - Milestones: firsts are DETECTED from what the fish actually did (the
 *    model's own choices), per fish and per tank; render shows them.
 *  - Habits: the tank remembers where you feed and greets you at light-on.
 *  - Ravenous boot: powered off >= 1 h -> fish are starving on return and wait
 *    near the surface (at your usual feeding spot) until the first feeding.
 *    Nothing else is simulated for time away; nothing ever dies.
 *  - Saves: on events (arrival, stage, milestone, light change), coalesced to
 *    >= 30 s apart, plus a 10-minute heartbeat - NVS-friendly. */
#ifndef PROGRESSION_H
#define PROGRESSION_H
#include "tank.h"
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* ---- platform ports (sim: file + time(); device: NVS + RTC) ---- */
bool    persist_port_load(void *buf, size_t len);          /* false = nothing saved */
bool    persist_port_save(const void *buf, size_t len);
int64_t clock_port_now_unix(void);                         /* 0 if unknown */

/* call after tank_init: restores the saved tank (or creates a new population)
 * and applies the boot rule */
void progression_boot(tank_t *t);
/* call every frame after tank_tick */
void progression_tick(tank_t *t, float dt);
/* call on light-off / shutdown (autosaves on events + heartbeat anyway) */
void progression_save(tank_t *t);

/* sim/debug: multiply tended time (aging, drift) - `./fishsim --fast 60` */
extern float progression_time_scale;
/* debug: stage and show an arrival now (sim key R); no-op at the cap */
void progression_force_arrival(tank_t *t);
bool progression_arrival_pending(void);
float progression_age_s(const tank_t *t, int idx);        /* tended seconds */

/* population ceiling. Compile-time so the device can ship lower until its
 * advisor latency is measured (docs/progression-next.md): firmware passes
 * -DPOP_CAP=5, the sim shows all 6. Never above N_FISH_MAX. */
#ifndef POP_CAP
#define POP_CAP N_FISH_MAX
#endif

/* stage thresholds, in seconds of tended life (fish age only while the tank
 * runs with the light on). A desk companion is glanced at over weeks, so the
 * arc is hours/days, not minutes. */
#define STAGE_JUV_AGE    (1 * 3600)
#define STAGE_ADULT_AGE  (6 * 3600)
#define STAGE_ELDER_AGE  (48 * 3600)
/* trait drift: one full unit (0.11 of the 0..1 trait) per this many hours of
 * sustained pressure */
#define DRIFT_HOURS      2.0f

/* milestone labels (UI / logs); order = bit order in tank.h */
extern const char *const MS_NAMES[MS_FISH_COUNT];
extern const char *const TMS_NAMES[TMS_COUNT];
#endif
