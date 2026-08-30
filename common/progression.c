#include "progression.h"
#include <string.h>
#include <math.h>

#define SAVE_MAGIC 0x50544b32u   /* "PTK2" (PTK1 saves are 4-fish, pre-population: start fresh) */
#define RAVENOUS_AFTER_S (60 * 60)
#define RAVENOUS_GIVE_UP_S 150.0f  /* begging window before the fish give up */
#define SAVE_HEARTBEAT_S 600.0f
#define SAVE_MIN_GAP_S   30.0f

const char *const MS_NAMES[MS_FISH_COUNT] = {
    "arrived", "first meal from you", "first hold-approach", "first dart", "first bubbles",
    "first reef", "first shadow survived", "first shrug", "first follow",
    "reached juv", "reached adult", "reached elder",
};
const char *const TMS_NAMES[TMS_COUNT] = {
    "a pair", "a trio", "a quartet", "a quintet", "a sextet",
    "first quiet night", "first play session", "the tank changed someone", "first feeding",
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
} save_t;

float progression_time_scale = 1.0f;

static float s_age[N_FISH_MAX];      /* seconds of tended life per fish */
static float s_shrug_t[N_FISH_MAX];  /* seconds holding a non-flee goal with a shadow mid-range */
static bool  s_threatened[N_FISH_MAX];
static float s_since_save, s_dirty_since;
static bool  s_dirty;
static bool  s_ravenous;             /* begging active until first feeding / give-up */
static float s_ravenous_t;           /* seconds spent begging */
static bool  s_arrival_pending;
static bool  s_prev_night;
static bool  s_booted;

static float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }
static void mark_dirty(void) { if (!s_dirty) { s_dirty = true; s_dirty_since = 0; } }

float progression_age_s(const tank_t *t, int idx) { (void)t; return idx >= 0 && idx < N_FISH_MAX ? s_age[idx] : 0; }
bool  progression_arrival_pending(void) { return s_arrival_pending; }

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
    static const float stage_scale[4] = { 0.55f, 0.78f, 1.0f, 1.08f };
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
    s_age[slot] = 0; s_shrug_t[slot] = 0; s_threatened[slot] = false;
    t->fish[slot].ms_bits = MS_ARRIVED;
    if (t->n_fish <= N_FISH_MAX) set_tms(t, POP_TMS[t->n_fish]);
    mark_dirty();
}

/* care gates (docs/progression-next.md, Act 2): never time alone */
static bool arrival_earned(const tank_t *t) {
    if (t->n_fish >= POP_CAP || t->n_fish >= N_FISH_MAX) return false;
    float min_trust = 10; bool changed = false;
    for (int i = 0; i < t->n_fish; i++) {
        const fish_t *f = &t->fish[i];
        if (f->trust < min_trust) min_trust = f->trust;
        if (fabsf(f->bold - f->bold0) >= 0.11f || fabsf(f->sociable - f->sociable0) >= 0.11f) changed = true;
    }
    const fish_t *last = &t->fish[t->n_fish - 1];
    switch (t->n_fish) {
    case 2:  return min_trust >= 6.0f && t->player_feedings >= 12 && t->hold_approaches >= 1;
    case 3:  return last->stage >= STAGE_JUV && (changed || t->player_feedings >= 40);
    case 4:  return last->stage >= STAGE_ADULT && t->player_feedings >= 80 && min_trust >= 7.0f;
    default: return last->stage >= STAGE_ADULT && t->player_feedings >= 140 && min_trust >= 8.0f;
    }
}

void progression_force_arrival(tank_t *t) { s_arrival_pending = true; do_arrival(t); }

void progression_boot(tank_t *t) {
    save_t sv; memset(&sv, 0, sizeof sv);
    s_booted = true;
    if (!persist_port_load(&sv, sizeof sv) || sv.magic != SAVE_MAGIC || sv.n_fish < 2 || sv.n_fish > N_FISH_MAX) {
        tank_new_population(t);                                  /* a new tank: two adults */
        for (int i = 0; i < N_FISH_MAX; i++) { s_age[i] = STAGE_ADULT_AGE; s_shrug_t[i] = 0; s_threatened[i] = false; }
        t->tank_ms_bits = TMS_PAIR;
        for (int i = 0; i < t->n_fish; i++) t->fish[i].ms_bits = MS_ARRIVED | MS_REACHED_JUV | MS_REACHED_ADULT;
        s_arrival_pending = false; s_prev_night = t->night;
        mark_dirty();
        return;
    }
    t->n_fish = 0;
    for (int i = 0; i < sv.n_fish; i++) {
        const fish_save_t *s = &sv.fish[i];
        tank_make_fish(t, i, s->preset % tank_roster_count(), s->sociable, s->bold, (stage_t)(s->stage & 3));
        fish_t *f = &t->fish[i];
        f->trust = s->trust; f->bold0 = s->bold0; f->sociable0 = s->sociable0;
        f->hunger = s->hunger; f->energy = s->energy; f->stress = s->stress; f->curiosity = s->curiosity;
        f->eaten = s->eaten; f->eaten_player = s->eaten_player; f->ms_bits = s->ms_bits;
        f->rest_dx = s->rest_dx; f->rest_dy = s->rest_dy;
        s_age[i] = s->age_s; s_shrug_t[i] = 0; s_threatened[i] = false;
        t->n_fish = i + 1;
    }
    t->light_override = sv.light_override; t->light_on = sv.light_on;
    t->feed_spot_x = sv.feed_spot_x; t->player_feedings = sv.player_feedings;
    t->hold_approaches = sv.hold_approaches; t->tank_ms_bits = sv.tank_ms_bits;
    s_arrival_pending = sv.arrival_pending;
    s_prev_night = t->night;
    int64_t now = clock_port_now_unix();
    if (now > 0 && sv.saved_unix > 0 && now - sv.saved_unix >= RAVENOUS_AFTER_S) {
        s_ravenous = true; s_ravenous_t = 0;                 /* the one offline rule */
        for (int i = 0; i < t->n_fish; i++) t->fish[i].hunger = 9.6f;
    }
    if (s_arrival_pending && !t->night) do_arrival(t);       /* earned while you were away: here it is */
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
        if (t->shadow.active) {
            float sd = tank_dist(f->x, f->y, t->shadow.x, t->shadow.y);
            if (sd < 115) s_threatened[i] = true;
            if (sd >= 70 && sd < 180 && f->goal.id != GOAL_FLEE_SHADOW) {
                s_shrug_t[i] += dt;
                if (s_shrug_t[i] > 3.0f) set_ms(f, MS_FIRST_SHRUG);   /* in view, chose not to flee */
            } else s_shrug_t[i] = 0;
        } else {
            if (s_threatened[i]) { set_ms(f, MS_FIRST_SHADOW_SURVIVED); s_threatened[i] = false; }
            s_shrug_t[i] = 0;
        }
        if (f->goal.id == GOAL_REST) n_rest++;
        if (f->goal.id == GOAL_DART_PLAY) n_dart++;
    }
    if (t->n_fish >= 2 && n_rest == t->n_fish && t->night) set_tms(t, TMS_FIRST_QUIET_NIGHT);
    if (n_dart >= 2) set_tms(t, TMS_FIRST_PLAY_SESSION);
    if (changed_someone) set_tms(t, TMS_CHANGED_SOMEONE);
    if (t->player_feedings > 0) set_tms(t, TMS_FIRST_FEEDING);

    /* ravenous: a starving tank with empty water begs at the surface (tank.c
     * renders the wait; the trickle holds off so the keeper's pellets are the
     * event). Entered at boot after a long absence, or live whenever everyone
     * is starving - waking from device sleep lands here naturally. If nobody
     * comes, after a while the fish give up and the tank feeds itself. */
    int any_food = 0; for (int i = 0; i < MAX_FOOD; i++) any_food |= t->food[i].alive;
    if (!s_ravenous && !any_food && t->n_fish > 0) {
        float mn = 10;
        for (int i = 0; i < t->n_fish; i++) if (t->fish[i].hunger < mn) mn = t->fish[i].hunger;
        if (mn >= 8.5f) { s_ravenous = true; s_ravenous_t = 0; }
    }
    if (s_ravenous) {
        s_ravenous_t += dt;
        int ate = 0; for (int i = 0; i < t->n_fish; i++) ate |= (t->fish[i].hunger < 6);
        if (ate) s_ravenous = false;                          /* first feeding ends it */
        else if (s_ravenous_t > RAVENOUS_GIVE_UP_S) {         /* nobody came: back to life */
            s_ravenous = false;
            tank_scatter_food(t, 2);                          /* so it doesn't re-trigger at once */
        }
    }
    t->ravenous = s_ravenous && !any_food;

    /* light-on: greet, and show a staged arrival */
    bool light_on_edge = s_prev_night && !t->night;
    if (s_prev_night != t->night) mark_dirty();
    s_prev_night = t->night;
    if (light_on_edge) {
        t->greet_timer = 6.0f;
        if (s_arrival_pending) do_arrival(t);
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
    for (int i = 0; i < t->n_fish; i++) {
        const fish_t *f = &t->fish[i]; fish_save_t *s = &sv.fish[i];
        s->preset = (uint8_t)f->preset; s->stage = (uint8_t)f->stage;
        s->size = f->size; s->trust = f->trust; s->bold = f->bold; s->sociable = f->sociable;
        s->bold0 = f->bold0; s->sociable0 = f->sociable0;
        s->hunger = f->hunger; s->energy = f->energy; s->stress = f->stress; s->curiosity = f->curiosity;
        s->age_s = s_age[i]; s->rest_dx = f->rest_dx; s->rest_dy = f->rest_dy;
        s->eaten = f->eaten; s->eaten_player = f->eaten_player; s->ms_bits = f->ms_bits;
    }
    persist_port_save(&sv, sizeof sv);
    s_since_save = 0; s_dirty = false; s_dirty_since = 0;
}
