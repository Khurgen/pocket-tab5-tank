/* progression.h — the "shaped by attention, never ruined by absence" layer
 * (docs/progression.md + docs/progression-next.md). Platform-agnostic;
 * persistence and wall-clock come in through two tiny port functions.
 *
 *  - Population: a new tank is two contrasting adults; the 3rd..POP_CAP-th
 *    fish ARRIVE as fry when care milestones are met (trust, feedings, a
 *    calm hold, a raised fry, drift). An arrival is staged when earned and
 *    shown at the next light-on (or boot) - an unannounced surprise.
 *  - Growth: well-fed fish grow (size) and advance fry -> juv -> adult -> elder
 *    with time - every awake second, light on or off (2026-09-14, Strato:
 *    "fish don't stop growing with the light off"), and a slept span at
 *    SLEEP_GROWTH_FRAC of its length; stage changes are silent surprises.
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
bool    persist_port_erase(void);                          /* EVERY saved tank, parked copies included */
int64_t clock_port_now_unix(void);                         /* 0 if unknown */

/* call after tank_init: restores the saved tank (or creates a new population)
 * and applies the boot rule */
void progression_boot(tank_t *t);
/* the device waking from DEEP sleep (2026-09-14): restore the save, then live
 * through the dark stretch since it was written - tank_tick_sleep for
 * (now_unix - saved_unix), capped at PROGRESSION_SLEEP_CAP_S - instead of the
 * cold-boot ravenous rule. Hunger, energy, grass and algae land where a night
 * of drowse would have put them; a long enough night ends in ravenous begging
 * by itself. Returns the hours simulated, or -1 when no clock was available
 * (then it behaved like progression_boot without the ravenous rule). */
#define PROGRESSION_SLEEP_CAP_S (7 * 24 * 3600)
/* how much of a slept span the fish grow through (2026-09-14, Strato: "an
 * eight-hour sleep should be worth 2 hours of growth") */
#define SLEEP_GROWTH_FRAC 0.25f
float progression_wake(tank_t *t, int64_t now_unix);
/* call every frame after tank_tick */
void progression_tick(tank_t *t, float dt);
/* call on light-off / shutdown (autosaves on events + heartbeat anyway) */
void progression_save(tank_t *t);

/* sim/debug: multiply time (aging, drift) - `./fishsim --fast 60` */
extern float progression_time_scale;
/* debug: stage and show an arrival now (sim key R); no-op at the cap */
void progression_force_arrival(tank_t *t);
/* debug: stage an arrival WITHOUT showing it - the courtship tell runs and
 * the fry appears at the next light-on (or progression_force_arrival) */
void progression_stage_arrival(tank_t *t);
bool progression_arrival_pending(void);
float progression_age_s(const tank_t *t, int idx);        /* grown seconds */
/* director/debug: put a fish's tended clock at `seconds` and apply the stage
 * and size that go with it now (the silent surprise, on cue) */
void progression_set_age(tank_t *t, int idx, float seconds);
/* director/debug: the new-tank path on a tank_init'd tank - two fry, first
 * milestones, nothing tended yet. Saves over the current save on the next
 * heartbeat: stash it first (firmware director: `stash`). */
void progression_fresh(tank_t *t);
/* the keeper's RESET (device: hold BOOT + tap the glass, then YES on the
 * prompt; sim: X): every save is erased - a director-parked tank too - the
 * tank is re-initialised with `seed`, the new-tank path runs and the fresh
 * pair is saved at once, so a reboot lands on them. Two fry, clean glass,
 * the default garden, nothing tended yet. */
void progression_reset(tank_t *t, uint32_t seed);
/* first-run setup (setup.c, 2026-09-13): a tank born through
 * progression_fresh - a fresh install, a reset - is PENDING setup until the
 * keeper walks the welcome / names / colours flow; the flag rides in the
 * save, so a reboot mid-setup re-opens it. progression_setup_done clears it
 * and saves the names and looks at once. Tanks saved before the flag read as
 * done (nobody's running tank gets the tutorial after an update). */
bool progression_setup_pending(void);
void progression_setup_done(tank_t *t);
/* the birth flow (setup.c, 2026-09-14): every arrival is owed a visit - the
 * announcement, a name, the family page - until the keeper walks it.
 * progression_newborn is the slot still owed (-1 = none); it rides in the
 * save, so a reboot mid-flow brings it back. progression_newborn_done clears
 * it and saves the name at once. */
int  progression_newborn(void);
void progression_newborn_done(tank_t *t);
/* milestones page (2026-09-13): the keeper closed it - everything earned so
 * far counts as seen (badges earned later wear a "new" ring until the next
 * look). Rides in the save. */
void progression_ack_milestones(tank_t *t);
/* the new-fry checklist (2026-09-14, Strato: once the pair starts to grow,
 * the first thing a keeper asks is "how do I get a new fry?" - and the
 * milestones page said nothing). progression_next_fry fills one line per
 * gate of the NEXT arrival: the care gates arrival_conditions counts (the
 * same code, so the list can never disagree with the rule) plus the nursery
 * bed every arrival needs. Each line: a kind (the renderer picks the art),
 * a title, the words (what to do - a plain sentence over two lines; Strato:
 * "all fish must have a minimum six out of 10 trust score", not a hint), a
 * progress phrase (where it stands), a 0..1 fraction and whether it is met. Returns the count, 0 at the
 * population cap. *staged = every gate is met and the fry waits for the
 * next light-on. Strings fit the pixel font: <= 25 chars at scale 2. */
enum { FRY_REQ_TRUST, FRY_REQ_FEED, FRY_REQ_HOLD, FRY_REQ_GROW, FRY_REQ_CHANGE, FRY_REQ_GRASS };
#define FRY_REQ_MAX 4
typedef struct {
    int   kind;
    char  title[12];
    char  words[28], words2[28];   /* what to do, a plain sentence over two lines */
    char  progress[28];            /* where it stands */
    float frac;
    bool  met;
} fry_req_t;
int progression_next_fry(const tank_t *t, fry_req_t out[FRY_REQ_MAX], bool *staged);
/* the tip behind a gate (Strato: "if someone reads 'all fish must have a
 * trust of at least six' they may ask OK how do I do that?"): HOW the
 * keeper moves it, in up to FRY_TIP_LINES lines of <= 26 chars, NULL-
 * terminated. Written from the rules in tank.c / progression.c (trust
 * only rises under a resting finger; quick taps cost it; fish age only
 * while lit; grass regrows by itself). */
#define FRY_TIP_LINES 5
const char *const *progression_fry_tip(int kind);

/* population ceiling. Compile-time so the device can ship lower until its
 * advisor latency is measured (docs/progression-next.md): firmware passes
 * -DPOP_CAP=5, the sim shows all 6. Never above N_FISH_MAX. */
#ifndef POP_CAP
#define POP_CAP N_FISH_MAX
#endif

/* stage thresholds, in seconds of grown life: awake time in full (the light
 * makes no difference), sleep at SLEEP_GROWTH_FRAC. A desk companion is
 * glanced at over weeks, so the arc is hours/days, not minutes. */
/* tended seconds to each stage (2026-08-30, halved from 1/6/48 h: a fresh
 * tank now starts as FRY, so the first growth spurt lands in the keeper's
 * first session and elder stays a multi-day achievement) */
#define STAGE_JUV_AGE    (30 * 60)
#define STAGE_ADULT_AGE  (3 * 3600)
#define STAGE_ELDER_AGE  (24 * 3600)
/* trait drift: one full unit (0.11 of the 0..1 trait) per this many hours of
 * sustained pressure */
#define DRIFT_HOURS      2.0f

/* milestone labels (UI / logs); order = bit order in tank.h */
extern const char *const MS_NAMES[MS_FISH_COUNT];
extern const char *const TMS_NAMES[TMS_COUNT];
#endif
