#include "progression.h"
#include <stddef.h>
#include <string.h>
#include <math.h>

#define SAVE_MAGIC 0x50544b32u   /* "PTK2" (PTK1 saves are 4-fish, pre-population: start fresh) */
#define RAVENOUS_AFTER_S (60 * 60)
#define RAVENOUS_GIVE_UP_S 150.0f  /* begging window before the fish give up */
#define SAVE_HEARTBEAT_S 600.0f
#define SAVE_MIN_GAP_S   30.0f

const char *const MS_NAMES[MS_FISH_COUNT] = {
    "arrived", "first meal from you", "first hold-approach", "first dart", "first bubbles",
    "first reef", "(retired)", "(retired)", "first follow",
    "reached juv", "reached adult", "reached elder",
};
const char *const TMS_NAMES[TMS_COUNT] = {
    "a pair", "a trio", "a quartet", "a quintet", "a sextet",
    "first quiet night", "first play session", "the tank changed someone", "first feeding",
    "first trimming", "first glass cleaning",
};

typedef struct {
    uint8_t preset, stage; uint16_t pad;
    float size, trust, bold, sociable, bold0, sociable0, hunger, energy, stress, curiosity;
    float age_s, rest_dx, rest_dy;
    int32_t eaten, eaten_player;
    uint32_t ms_bits;
} fish_save_t;

typedef struct {
    uint32_t magic;
    int64_t  saved_unix;
    float    clock;
    bool     light_override, light_on, arrival_pending;
    uint8_t  n_fish;
    float    feed_spot_x;
    int32_t  player_feedings, hold_approaches;
    uint32_t tank_ms_bits;
    fish_save_t fish[N_FISH_MAX];
    /* ---- upkeep tail (2026-08-30). Fields only ever APPEND here: boot falls
     * back to loading the prefix above from an older PTK2 save; the zeroed
     * tail then reads as clean glass and default vegetation (0 is not a legal
     * growth - the floor is VEG_NUB - so restore treats it as "keep the
     * fresh-tank default"). ---- */
    float    veg_growth[VEG_BEDS];
    uint8_t  algae[ALGAE_CELLS];
    int32_t  trims, cells_cleaned;
    /* per-frond heights (2026-09-04); an older save (no tail, or zeros)
     * seeds every frond from its bed's veg_growth */
    float    veg_h[VEG_BEDS][VEG_FRONDS_MAX];
    /* identity tail (2026-09-13, first-run setup): the keeper's names and
     * colours per fish - an empty name / a zero colour = the preset's - and
     * whether the setup flow still owes the keeper a visit. Older saves read
     * zeros: preset looks, setup done. */
    uint8_t  setup_pending, pad_id[3];
    char     names[N_FISH_MAX][FISH_NAME_MAX + 1];
    uint32_t body[N_FISH_MAX], accent[N_FISH_MAX];
    /* seen-milestones tail (2026-09-13, the milestones page): what the
     * keeper has already looked at, so a badge earned since wears a ring.
     * Older saves read zeros: everything earned shows as new once. */
    uint32_t ms_seen[N_FISH_MAX], tank_ms_seen;
} save_t;
#define SAVE_CORE_SIZE   offsetof(save_t, veg_growth)   /* pre-upkeep PTK2 size */
#define SAVE_UPKEEP_SIZE offsetof(save_t, veg_h)        /* 2026-08-30 .. 09-04 size */
#define SAVE_FROND_SIZE  offsetof(save_t, setup_pending) /* 2026-09-04 .. 09-13 size */
#define SAVE_IDENT_SIZE  offsetof(save_t, ms_seen)       /* identity tail, before the seen masks */

float progression_time_scale = 1.0f;

static float s_age[N_FISH_MAX];      /* seconds of tended life per fish */
static float s_since_save, s_dirty_since;
static bool  s_dirty;
static bool  s_ravenous;             /* begging/frenzy active until everyone's fed / give-up */
static float s_ravenous_t;           /* seconds spent begging (dash time excluded) */
static int   s_rav_feedings0;        /* player_feedings when the episode began (tank.ravenous_fed) */
static bool  s_arrival_pending;
static bool  s_prev_night;
static bool  s_booted;
static bool  s_setup_pending;        /* the first-run flow still owed (setup.c) */

static float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }
static void mark_dirty(void) { if (!s_dirty) { s_dirty = true; s_dirty_since = 0; } }

float progression_age_s(const tank_t *t, int idx) { (void)t; return idx >= 0 && idx < N_FISH_MAX ? s_age[idx] : 0; }
bool  progression_arrival_pending(void) { return s_arrival_pending; }
bool  progression_setup_pending(void)   { return s_setup_pending; }
void  progression_setup_done(tank_t *t) { s_setup_pending = false; progression_save(t); }

static void set_ms(fish_t *f, uint32_t bit) { if (!(f->ms_bits & bit)) { f->ms_bits |= bit; mark_dirty(); } }
static void set_tms(tank_t *t, uint32_t bit) { if (!(t->tank_ms_bits & bit)) { t->tank_ms_bits |= bit; mark_dirty(); } }

static void apply_stage(fish_t *f, float age) {
    stage_t st = age >= STAGE_ELDER_AGE ? STAGE_ELDER : age >= STAGE_ADULT_AGE ? STAGE_ADULT
               : age >= STAGE_JUV_AGE ? STAGE_JUV : STAGE_FRY;
    if (st != f->stage) { f->stage = st; mark_dirty(); }         /* silent surprise */
    if (st >= STAGE_JUV)   set_ms(f, MS_REACHED_JUV);
    if (st >= STAGE_ADULT) set_ms(f, MS_REACHED_ADULT);
    if (st >= STAGE_ELDER) set_ms(f, MS_REACHED_ELDER);
}

/* size: base by stage, plus a meal-fed bonus; never shrinks below stage base */
static void apply_growth(fish_t *f) {
    static const float stage_scale[4] = { 0.55f, 0.78f, 1.04f, 1.23f };  /* elder +14% (2026-08-30) */
    float fed = 1.0f + clampf(f->eaten / 60.0f, 0, 1) * 0.18f;
    f->size = f->base_size * stage_scale[f->stage] * fed;
}

static const uint32_t POP_TMS[N_FISH_MAX + 1] = { 0, 0, TMS_PAIR, TMS_TRIO, TMS_QUARTET, TMS_QUINTET, TMS_SEXTET };

/* the arrival itself: a fry by the reef, traits inherited from the two most
 * trusting adults; the stage clock starts from zero */
static void do_arrival(tank_t *t) {
    int a = -1, b = -1;
    for (int i = 0; i < t->n_fish; i++) {
        if (t->fish[i].stage < STAGE_ADULT) continue;
        if (a < 0 || t->fish[i].trust > t->fish[a].trust) { b = a; a = i; }
        else if (b < 0 || t->fish[i].trust > t->fish[b].trust) b = i;
    }
    int slot = tank_add_fish(t, a, b);
    s_arrival_pending = false;
    if (slot < 0) return;
    int nb = tank_nursery_bed(t);            /* born in the grass it was courted in */
    if (nb >= 0) {
        float x0, x1; tank_veg_bed(t, nb, &x0, &x1, NULL, NULL);
        t->fish[slot].x = (x0 + x1) * 0.5f; t->fish[slot].y = TANK_H - 16 - 18;
    }
    s_age[slot] = 0;
    t->fish[slot].ms_bits = MS_ARRIVED;
    if (t->n_fish <= N_FISH_MAX) set_tms(t, POP_TMS[t->n_fish]);
    mark_dirty();
}

/* care gates (docs/progression-next.md, Act 2): never time alone.
 * Counted, not just checked, so the tank can TELL when it's close: one
 * condition shy of an arrival, the parents-to-be start courting. */
static void arrival_conditions(const tank_t *t, int *met, int *total) {
    float min_trust = 10; bool changed = false;
    for (int i = 0; i < t->n_fish; i++) {
        const fish_t *f = &t->fish[i];
        if (f->trust < min_trust) min_trust = f->trust;
        if (fabsf(f->bold - f->bold0) >= 0.11f || fabsf(f->sociable - f->sociable0) >= 0.11f) changed = true;
    }
    const fish_t *last = &t->fish[t->n_fish - 1];
    switch (t->n_fish) {
    case 2:  *total = 3; *met = (min_trust >= 6.0f) + (t->player_feedings >= 12) + (t->hold_approaches >= 1); break;
    case 3:  *total = 2; *met = (last->stage >= STAGE_JUV) + (changed || t->player_feedings >= 40); break;
    case 4:  *total = 3; *met = (last->stage >= STAGE_ADULT) + (t->player_feedings >= 80) + (min_trust >= 7.0f); break;
    default: *total = 3; *met = (last->stage >= STAGE_ADULT) + (t->player_feedings >= 140) + (min_trust >= 8.0f); break;
    }
}

static bool arrival_earned(const tank_t *t) {
    if (t->n_fish >= POP_CAP || t->n_fish >= N_FISH_MAX) return false;
    if (tank_nursery_bed(t) < 0) return false;   /* no grass to be born in */
    int met, total;
    arrival_conditions(t, &met, &total);
    return met == total;
}

void progression_force_arrival(tank_t *t) { s_arrival_pending = true; do_arrival(t); }
void progression_stage_arrival(tank_t *t) { (void)t; if (!s_arrival_pending) { s_arrival_pending = true; mark_dirty(); } }

void progression_fresh(tank_t *t) {
    tank_new_population(t);          /* a new tank: two FRY, contrasting -
                                      * the keeper watches them grow up */
    for (int i = 0; i < N_FISH_MAX; i++) s_age[i] = 0;
    t->tank_ms_bits = TMS_PAIR;
    for (int i = 0; i < t->n_fish; i++) { t->fish[i].ms_bits = MS_ARRIVED; apply_growth(&t->fish[i]); }
    s_arrival_pending = false; s_prev_night = t->night;
    s_ravenous = false; s_ravenous_t = 0;
    s_setup_pending = true;          /* a new tank: welcome, names, colours */
    s_booted = true;
    mark_dirty();
}

void progression_reset(tank_t *t, uint32_t seed) {
    persist_port_erase();
    tank_init(t, seed);
    progression_fresh(t);
    progression_save(t);
}

void progression_set_age(tank_t *t, int idx, float seconds) {
    if (idx < 0 || idx >= t->n_fish) return;
    s_age[idx] = seconds < 0 ? 0 : seconds;
    apply_stage(&t->fish[idx], s_age[idx]);
    apply_growth(&t->fish[idx]);
    mark_dirty();
}

void progression_boot(tank_t *t) {
    save_t sv; memset(&sv, 0, sizeof sv);
    s_booted = true;
    bool loaded = persist_port_load(&sv, sizeof sv);
    if (!loaded) {                       /* pre-seen-masks save: load that prefix */
        memset(&sv, 0, sizeof sv);
        loaded = persist_port_load(&sv, SAVE_IDENT_SIZE);
    }
    if (!loaded) {                       /* pre-identity save: load that prefix */
        memset(&sv, 0, sizeof sv);
        loaded = persist_port_load(&sv, SAVE_FROND_SIZE);
    }
    if (!loaded) {                       /* pre-frond upkeep save: load that prefix */
        memset(&sv, 0, sizeof sv);
        loaded = persist_port_load(&sv, SAVE_UPKEEP_SIZE);
    }
    if (!loaded) {                       /* pre-upkeep PTK2 save: load the prefix */
        memset(&sv, 0, sizeof sv);
        loaded = persist_port_load(&sv, SAVE_CORE_SIZE);
    }
    if (!loaded || sv.magic != SAVE_MAGIC || sv.n_fish < 2 || sv.n_fish > N_FISH_MAX) {
        progression_fresh(t);
        return;
    }
    t->n_fish = 0;
    for (int i = 0; i < sv.n_fish; i++) {
        const fish_save_t *s = &sv.fish[i];
        tank_make_fish(t, i, s->preset % tank_roster_count(), s->sociable, s->bold, (stage_t)(s->stage & 3));
        fish_t *f = &t->fish[i];
        f->trust = s->trust; f->bold0 = s->bold0; f->sociable0 = s->sociable0;
        f->hunger = s->hunger; f->energy = s->energy; f->stress = s->stress; f->curiosity = s->curiosity;
        f->eaten = s->eaten; f->eaten_player = s->eaten_player;
        f->ms_bits = s->ms_bits & ~MS_RETIRED_MASK;   /* the shadow milestones, gone with it */
        f->ms_seen = sv.ms_seen[i] & f->ms_bits;
        f->rest_dx = s->rest_dx; f->rest_dy = s->rest_dy;
        s_age[i] = s->age_s;
        t->n_fish = i + 1;
        if (sv.names[i][0]) { sv.names[i][FISH_NAME_MAX] = 0; tank_set_name(t, i, sv.names[i]); }
        tank_set_look(t, i, sv.body[i], sv.accent[i]);        /* zeros keep the preset's */
    }
    s_setup_pending = sv.setup_pending != 0;
    t->light_override = sv.light_override; t->light_on = sv.light_on;
    t->feed_spot_x = sv.feed_spot_x; t->player_feedings = sv.player_feedings;
    t->hold_approaches = sv.hold_approaches; t->tank_ms_bits = sv.tank_ms_bits;
    t->tank_ms_seen = sv.tank_ms_seen & t->tank_ms_bits;
    for (int b = 0; b < VEG_BEDS; b++) {
        if (sv.veg_h[b][0] > 0)
            for (int i = 0; i < VEG_FRONDS_MAX; i++) t->veg_h[b][i] = sv.veg_h[b][i];
        else if (sv.veg_growth[b] > 0) tank_veg_set(t, b, sv.veg_growth[b]);
    }
    tank_veg_sync(t);
    memcpy(t->algae, sv.algae, ALGAE_CELLS);
    t->trims = sv.trims; t->cells_cleaned = sv.cells_cleaned;
    s_arrival_pending = sv.arrival_pending;
    s_prev_night = t->night;
    int64_t now = clock_port_now_unix();
    if (now > 0 && sv.saved_unix > 0 && now - sv.saved_unix >= RAVENOUS_AFTER_S) {
        s_ravenous = true; s_ravenous_t = 0;                 /* the one offline rule */
        s_rav_feedings0 = t->player_feedings;
        for (int i = 0; i < t->n_fish; i++) t->fish[i].hunger = 9.6f;
    }
    if (s_arrival_pending && !t->night && tank_nursery_bed(t) >= 0) do_arrival(t);   /* earned while you were away: here it is */
}

void progression_tick(tank_t *t, float dt) {
    if (!s_booted) return;
    float tended = t->night ? 0 : dt * progression_time_scale;   /* lights off = paused */
    int n_rest = 0, n_dart = 0; bool changed_someone = false;
    for (int i = 0; i < t->n_fish; i++) {
        fish_t *f = &t->fish[i];
        s_age[i] += tended;
        apply_stage(f, s_age[i]);
        apply_growth(f);
        /* trait drift, slow: calm + fed -> bolder/more social; startled -> shyer */
        float k = tended / (DRIFT_HOURS * 3600.0f);          /* full unit per DRIFT_HOURS of pressure */
        if (f->stress > 7) f->bold = clampf(f->bold - k, 0.05f, 0.95f);
        else if (f->hunger < 4 && f->stress < 2) f->bold = clampf(f->bold + k * 0.5f, 0.05f, 0.95f);
        if (f->goal.id == GOAL_FOLLOW_FRIEND) f->sociable = clampf(f->sociable + k * 0.5f, 0.05f, 0.95f);
        else if (f->goal.id == GOAL_EXPLORE) f->sociable = clampf(f->sociable - k * 0.2f, 0.05f, 0.95f);
        if (fabsf(f->bold - f->bold0) >= 0.11f || fabsf(f->sociable - f->sociable0) >= 0.11f) changed_someone = true;

        /* milestones: firsts the fish chose to do */
        if (f->goal.id == GOAL_DART_PLAY && f->goal_age > 1.0f) set_ms(f, MS_FIRST_DART);
        if (f->goal.id == GOAL_VISIT_BUBBLES && tank_dist(f->x, f->y, t->bubble_x, t->bubble_y) < 90) set_ms(f, MS_FIRST_BUBBLES);
        if (f->goal.id == GOAL_INSPECT_REEF && tank_dist(f->x, f->y, t->reef_x, t->reef_y) < 90) set_ms(f, MS_FIRST_REEF);
        if (f->goal.id == GOAL_FOLLOW_FRIEND && f->goal_age > 2.0f) set_ms(f, MS_FIRST_FOLLOW);
        if (f->goal.id == GOAL_REST) n_rest++;
        if (f->goal.id == GOAL_DART_PLAY) n_dart++;
    }
    if (t->n_fish >= 2 && n_rest == t->n_fish && t->night) set_tms(t, TMS_FIRST_QUIET_NIGHT);
    if (n_dart >= 2) set_tms(t, TMS_FIRST_PLAY_SESSION);
    if (changed_someone) set_tms(t, TMS_CHANGED_SOMEONE);
    if (t->player_feedings > 0) set_tms(t, TMS_FIRST_FEEDING);
    /* upkeep milestones + event saves (a chore done deserves to stick) */
    static int32_t s_prev_trims, s_prev_cleaned;
    if (t->trims > 0) set_tms(t, TMS_FIRST_TRIM);
    if (t->cells_cleaned >= 30) set_tms(t, TMS_FIRST_CLEANING);
    if (t->trims != s_prev_trims || t->cells_cleaned != s_prev_cleaned) {
        s_prev_trims = t->trims; s_prev_cleaned = t->cells_cleaned;
        mark_dirty();
    }

    /* ravenous: a starving tank with empty water begs at the surface (tank.c
     * renders the wait; the trickle holds off so the keeper's pellets are the
     * event). Entered at boot after a long absence, or live whenever everyone
     * is starving - waking from device sleep lands here naturally. If nobody
     * comes, after a while the fish give up and the tank feeds itself. */
    int any_food = 0; for (int i = 0; i < MAX_FOOD; i++) any_food |= t->food[i].alive;
    if (!s_ravenous && !any_food && t->n_fish > 0) {
        float mn = 10;
        for (int i = 0; i < t->n_fish; i++) if (t->fish[i].hunger < mn) mn = t->fish[i].hunger;
        if (mn >= 8.5f) { s_ravenous = true; s_ravenous_t = 0; s_rav_feedings0 = t->player_feedings; }
    }
    if (s_ravenous) {
        if (!any_food) s_ravenous_t += dt;    /* the wait; a dash for live pellets isn't giving up */
        float mx = 0;
        for (int i = 0; i < t->n_fish; i++) if (t->fish[i].hunger > mx) mx = t->fish[i].hunger;
        if (mx < 7.0f) s_ravenous = false;                    /* everyone got a bite */
        else if (s_ravenous_t > RAVENOUS_GIVE_UP_S) {         /* nobody came: back to life */
            s_ravenous = false;
            tank_scatter_food(t, 2);                          /* so it doesn't re-trigger at once */
        }
    }
    /* tank.c picks the presentation: empty water = beg at the surface; live
     * pellets = feeding-frenzy dash (real starving fish DART at fresh food) */
    t->ravenous = s_ravenous;
    t->ravenous_fed = s_ravenous && t->player_feedings != s_rav_feedings0;

    /* the courtship tell: one condition shy of an arrival (or one staged),
     * the two most-trusting grown fish pair up - tank.c stages the episodes.
     * The pair is chosen exactly the way do_arrival picks parents. */
    t->courting = false; t->court_a = t->court_b = -1;
    if (t->n_fish < POP_CAP && t->n_fish < N_FISH_MAX && tank_nursery_bed(t) >= 0) {
        /* ... and only with a nursery: a bed tall enough to hide in. Shave
         * every bed and the courting stops until one regrows. */
        int met, total;
        arrival_conditions(t, &met, &total);
        if (s_arrival_pending || met >= total - 1) {
            int a = -1, b = -1;
            for (int i = 0; i < t->n_fish; i++) {
                if (t->fish[i].stage < STAGE_ADULT) continue;
                if (a < 0 || t->fish[i].trust > t->fish[a].trust) { b = a; a = i; }
                else if (b < 0 || t->fish[i].trust > t->fish[b].trust) b = i;
            }
            if (a >= 0 && b >= 0) { t->courting = true; t->court_a = (int8_t)a; t->court_b = (int8_t)b; }
        }
    }

    /* light-on: greet, and show a staged arrival */
    bool light_on_edge = s_prev_night && !t->night;
    if (s_prev_night != t->night) mark_dirty();
    s_prev_night = t->night;
    if (light_on_edge) {
        t->greet_timer = 6.0f;
        if (s_arrival_pending && tank_nursery_bed(t) >= 0) do_arrival(t);   /* the fry waits for grass */
    }
    if (!s_arrival_pending && arrival_earned(t)) { s_arrival_pending = true; mark_dirty(); }

    /* saves: coalesced event saves + heartbeat */
    s_since_save += dt; if (s_dirty) s_dirty_since += dt;
    if ((s_dirty && s_since_save >= SAVE_MIN_GAP_S) || s_since_save >= SAVE_HEARTBEAT_S)
        progression_save(t);
}

void progression_save(tank_t *t) {
    save_t sv; memset(&sv, 0, sizeof sv);
    sv.magic = SAVE_MAGIC; sv.saved_unix = clock_port_now_unix(); sv.clock = t->clock;
    sv.light_override = t->light_override; sv.light_on = t->light_on;
    sv.arrival_pending = s_arrival_pending; sv.n_fish = (uint8_t)t->n_fish;
    sv.feed_spot_x = t->feed_spot_x; sv.player_feedings = t->player_feedings;
    sv.hold_approaches = t->hold_approaches; sv.tank_ms_bits = t->tank_ms_bits;
    sv.tank_ms_seen = t->tank_ms_seen;
    for (int b = 0; b < VEG_BEDS; b++) {
        sv.veg_growth[b] = t->veg_growth[b];
        for (int i = 0; i < VEG_FRONDS_MAX; i++) sv.veg_h[b][i] = t->veg_h[b][i];
    }
    memcpy(sv.algae, t->algae, ALGAE_CELLS);
    sv.trims = t->trims; sv.cells_cleaned = t->cells_cleaned;
    sv.setup_pending = s_setup_pending;
    for (int i = 0; i < t->n_fish; i++) {
        const fish_t *f = &t->fish[i]; fish_save_t *s = &sv.fish[i];
        if (strcmp(f->name, tank_roster_name(f->preset))) memcpy(sv.names[i], f->name, FISH_NAME_MAX + 1);
        sv.body[i] = f->color; sv.accent[i] = f->accent;      /* the preset's too: harmless, exact */
        s->preset = (uint8_t)f->preset; s->stage = (uint8_t)f->stage;
        s->size = f->size; s->trust = f->trust; s->bold = f->bold; s->sociable = f->sociable;
        s->bold0 = f->bold0; s->sociable0 = f->sociable0;
        s->hunger = f->hunger; s->energy = f->energy; s->stress = f->stress; s->curiosity = f->curiosity;
        s->age_s = s_age[i]; s->rest_dx = f->rest_dx; s->rest_dy = f->rest_dy;
        s->eaten = f->eaten; s->eaten_player = f->eaten_player; s->ms_bits = f->ms_bits;
        sv.ms_seen[i] = f->ms_seen;
    }
    persist_port_save(&sv, sizeof sv);
    s_since_save = 0; s_dirty = false; s_dirty_since = 0;
}

void progression_ack_milestones(tank_t *t) {
    bool changed = false;
    for (int i = 0; i < t->n_fish; i++)
        if (t->fish[i].ms_seen != t->fish[i].ms_bits) { t->fish[i].ms_seen = t->fish[i].ms_bits; changed = true; }
    if (t->tank_ms_seen != t->tank_ms_bits) { t->tank_ms_seen = t->tank_ms_bits; changed = true; }
    if (changed) mark_dirty();
}
