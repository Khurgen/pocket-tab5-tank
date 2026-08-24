/* tank.c — reflex layer. See tank.h. Faithful port of the browser prototype's
 * updateFish/targetForGoal/wall handling, with prototype px values scaled by
 * ~0.55 for the 448-wide tank. */
#include "tank.h"
#include <math.h>

#define TAU 6.2831853f

const char *const GOAL_NAMES[GOAL_COUNT] = {
    "seek_food", "flee_shadow", "visit_bubbles", "follow_friend",
    "explore", "rest", "dart_play", "inspect_reef",
};

const char *const STAGE_NAMES[4] = { "fry", "juv", "adult", "elder" };
const char *const TRAINED_NAMES[N_TRAINED_NAMES] = { "mira", "bolt", "kelp", "nori" };

int tank_reflex_overrides = 0;   /* starvation-ignored episodes (diagnostic) */

/* deterministic xorshift32 */
static uint32_t xr(tank_t *t) {
    uint32_t x = t->rng;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    return t->rng = x;
}
float tank_randf(tank_t *t, float lo, float hi) {
    return lo + (hi - lo) * (float)(xr(t) & 0xffffff) / 16777215.0f;
}
static float clampf(float v, float lo, float hi) {
    return v < lo ? lo : v > hi ? hi : v;
}
static float norm_ang(float a) {
    while (a >  3.14159265f) a -= TAU;
    while (a < -3.14159265f) a += TAU;
    return a;
}
static float lerpf(float a, float b, float p) { return a + (b - a) * p; }

float tank_dist(float ax, float ay, float bx, float by) {
    float dx = ax - bx, dy = ay - by;
    return sqrtf(dx * dx + dy * dy);
}

int tank_nearest_food(const tank_t *t, const fish_t *f, float *dist_out) {
    int best = -1; float bd = 1e9f;
    for (int i = 0; i < MAX_FOOD; i++) {
        if (!t->food[i].alive) continue;
        float d = tank_dist(f->x, f->y, t->food[i].x, t->food[i].y);
        if (d < bd) { bd = d; best = i; }
    }
    if (dist_out) *dist_out = bd;
    return best;
}

int tank_nearest_friend(const tank_t *t, int fish_idx, float *dist_out) {
    const fish_t *f = &t->fish[fish_idx];
    int best = -1; float bd = 1e9f;
    for (int i = 0; i < t->n_fish; i++) {
        if (i == fish_idx) continue;
        float d = tank_dist(f->x, f->y, t->fish[i].x, t->fish[i].y);
        if (d < bd) { bd = d; best = i; }
    }
    if (dist_out) *dist_out = bd;
    return best;
}

/* ---- roster: six presets (the prototype's four + two), colors chosen for
 * contrast on AMOLED black. A preset is a look + temperament; bold/social are
 * rolled per tank (starting pair) or inherited (arrivals). ---- */
typedef struct {
    const char *name; uint32_t color, fin, accent;
    float size, curiosity, lazy, turn_rate;
} preset_t;
static const preset_t ROSTER[] = {
    /* name    color     fin       accent    size  cur  lazy  turn */
    { "mira", 0x38dcc7, 0x1d9f98, 0xffbd59, 1.08f, 7.5f, 0.2f, 3.2f },
    { "bolt", 0xff725c, 0xb83a43, 0xffe08a, 0.94f, 6.0f, 0.2f, 4.2f },
    { "kelp", 0x78d67d, 0x3f8b55, 0xa799ff, 0.86f, 4.0f, 0.1f, 3.2f },
    { "nori", 0xa799ff, 0x6b5ed6, 0x78d67d, 1.00f, 5.0f, 0.7f, 2.4f },
    { "pip",  0xffd166, 0xc98a1e, 0x38dcc7, 0.90f, 6.5f, 0.3f, 3.8f },
    { "sol",  0xf48fb1, 0xb0456f, 0xffe08a, 1.04f, 5.5f, 0.4f, 2.9f },
};
#define ROSTER_N ((int)(sizeof ROSTER / sizeof ROSTER[0]))
int tank_roster_count(void) { return ROSTER_N; }
const char *tank_roster_name(int preset) { return preset >= 0 && preset < ROSTER_N ? ROSTER[preset].name : "?"; }

void tank_make_fish(tank_t *t, int slot, int preset, float sociable, float bold, stage_t stage) {
    fish_t *f = &t->fish[slot];
    const preset_t *p = &ROSTER[preset];
    f->name = p->name; f->preset = preset;
    f->model_name = TRAINED_NAMES[slot % N_TRAINED_NAMES];   /* names carry no signal */
    f->x = tank_randf(t, 90, TANK_W - 90); f->y = tank_randf(t, 80, TANK_H - 90);
    f->heading = tank_randf(t, 0, TAU);
    f->speed = 0; f->target_speed = 0; f->wander = f->x * 0.05f;
    f->base_size = p->size; f->size = p->size; f->turn_rate = p->turn_rate;
    f->hunger = tank_randf(t, 3, 6); f->energy = tank_randf(t, 5, 8);
    f->stress = tank_randf(t, 0, 2); f->curiosity = p->curiosity;
    f->sociable = sociable; f->bold = bold; f->lazy = p->lazy;
    f->bold0 = bold; f->sociable0 = sociable;
    f->stage = stage;
    f->starve_flagged = false;
    f->trust = 5.0f;
    f->goal.id = GOAL_EXPLORE; f->goal.urgency = 3; f->goal.confidence = 1; f->goal.runner_up = GOAL_COUNT;
    f->goal_age = 0; f->ask_age = 99; f->dart_timer = 0; f->dart_x = f->x; f->dart_y = f->y; f->hesitate = 0;
    f->eaten = 0; f->eaten_player = 0;
    /* its own spot by the reef: bolder fish rest a little further out */
    f->rest_dx = 20 + slot * 9 + bold * 18; f->rest_dy = -slot * 7 - tank_randf(t, 0, 10);
    f->sig = 0xffffffffu; f->ms_bits = 0;
    f->color = p->color; f->fin = p->fin; f->accent = p->accent;
}

static void place_near_reef(tank_t *t, fish_t *f) {
    f->x = t->reef_x + tank_randf(t, -10, 30); f->y = t->reef_y - 30 + tank_randf(t, -10, 10);
    f->heading = tank_randf(t, -0.6f, 0.6f);
}

void tank_new_population(tank_t *t) {
    /* two random presets */
    int a = (int)tank_randf(t, 0, ROSTER_N - 0.001f);
    int b = (int)tank_randf(t, 0, ROSTER_N - 1.001f); if (b >= a) b++;
    /* personalities rolled with a guaranteed contrast so the pair reads as
     * two characters at a glance (docs/progression-next.md, Act 1) */
    float ba, bb, sa, sb;
    do { ba = tank_randf(t, 0.1f, 0.9f); bb = tank_randf(t, 0.1f, 0.9f); } while (fabsf(ba - bb) < 0.45f);
    do { sa = tank_randf(t, 0.1f, 0.9f); sb = tank_randf(t, 0.1f, 0.9f); } while (fabsf(sa - sb) < 0.3f);
    tank_make_fish(t, 0, a, sa, ba, STAGE_ADULT);
    tank_make_fish(t, 1, b, sb, bb, STAGE_ADULT);
    t->n_fish = 2;
}

int tank_add_fish(tank_t *t, int parent_a, int parent_b) {
    if (t->n_fish >= N_FISH_MAX || t->n_fish >= ROSTER_N) return -1;
    int used[ROSTER_N] = {0};
    for (int i = 0; i < t->n_fish; i++) used[t->fish[i].preset] = 1;
    int free_n = 0, free_idx[ROSTER_N];
    for (int i = 0; i < ROSTER_N; i++) if (!used[i]) free_idx[free_n++] = i;
    if (!free_n) return -1;
    int preset = free_idx[(int)tank_randf(t, 0, free_n - 0.001f)];
    int ia = (parent_a >= 0 && parent_a < t->n_fish) ? parent_a : 0;
    int ib = (parent_b >= 0 && parent_b < t->n_fish) ? parent_b : (t->n_fish > 1 ? 1 : 0);
    const fish_t *pa = &t->fish[ia], *pb = &t->fish[ib];
    float bold = clampf((pa->bold + pb->bold) * 0.5f + tank_randf(t, -0.15f, 0.15f), 0.05f, 0.95f);
    float soc  = clampf((pa->sociable + pb->sociable) * 0.5f + tank_randf(t, -0.15f, 0.15f), 0.05f, 0.95f);
    int slot = t->n_fish;
    tank_make_fish(t, slot, preset, soc, bold, STAGE_FRY);
    place_near_reef(t, &t->fish[slot]);
    t->fish[slot].hunger = 4; t->fish[slot].trust = 4;
    t->n_fish++;
    return slot;
}

void tank_init(tank_t *t, uint32_t seed) {
    t->rng = seed ? seed : 0xC0FFEE;
    t->n_fish = 0;                 /* progression_boot restores or calls tank_new_population */
    for (int i = 0; i < MAX_FOOD; i++) t->food[i].alive = false;
    for (int i = 0; i < MAX_BUBBLE; i++) {
        bubble_t *b = &t->bubble[i];
        b->column = i < 10;
        b->x = b->column ? TANK_W * 0.8f + tank_randf(t, -10, 10) : tank_randf(t, 12, TANK_W - 12);
        b->y = tank_randf(t, 0, TANK_H);
        b->vy = tank_randf(t, 14, 30);
        b->wobble = tank_randf(t, 0, TAU);
    }
    t->shadow.active = false; t->shadow.cool = 15;
    t->bubble_x = TANK_W * 0.8f;  t->bubble_y = TANK_H * 0.5f;   /* matches gen_traces.py */
    t->reef_x   = TANK_W * 0.15f; t->reef_y   = TANK_H * 0.85f;
    t->clock = 0; t->day_phase = 0; t->night = false;
    t->light_override = false; t->light_on = true;
    t->hold_active = false; t->hold_time = 0; t->hold_approached = false;
    t->tap_count = 0; t->tap_burst_t = 99; t->startled = false;
    t->startle_cooldown = 0;
    t->feed_spot_x = -1; t->player_feedings = 0; t->hold_approaches = 0; t->greet_timer = 0;
    t->tank_ms_bits = 0; t->ask_rr = 0; t->advisor_asks = 0;
    tank_scatter_food(t, 2);
}

#define TAP_WINDOW      0.5f    /* taps closer than this form a burst */
#define STARTLE_RADIUS  140.0f  /* fish this close to an aggressive tap bolt */
#define STARTLE_COOLDOWN 6.0f   /* calm seconds before the spook wears off */
#define HOLD_RADIUS     160.0f  /* fish this close notice a resting finger */

void tank_touch_hold(tank_t *t, float x, float y) {
    t->hold_active = true; t->hold_x = x; t->hold_y = y;
}

void tank_feed(tank_t *t, float x, int n) {
    x = clampf(x, 25, TANK_W - 25);
    for (int i = 0; i < MAX_FOOD && n > 0; i++) {
        if (t->food[i].alive) continue;
        t->food[i].alive = true; t->food[i].from_player = true;
        t->food[i].x = clampf(x + tank_randf(t, -14, 14), 20, TANK_W - 20);
        t->food[i].y = tank_randf(t, 6, 18);
        t->food[i].age = 0;
        n--;
    }
    t->feed_spot_x = t->feed_spot_x < 0 ? x : t->feed_spot_x + (x - t->feed_spot_x) * 0.3f;
    t->player_feedings++;
}

void tank_touch_tap(tank_t *t, float x, float y) {
    if (y < FEED_ZONE_Y) { tank_feed(t, x, 3); return; }     /* surface tap = feed */
    if (t->tap_burst_t > TAP_WINDOW) t->tap_count = 0;
    t->tap_count++; t->tap_burst_t = 0; t->tap_x = x; t->tap_y = y;
    if (t->startled) {                              /* chasing: keep them spooked */
        t->startle_x = x; t->startle_y = y; t->startle_cooldown = STARTLE_COOLDOWN;
        for (int i = 0; i < t->n_fish; i++) t->fish[i].stress = fminf(10, t->fish[i].stress + 0.6f);
    } else if (t->tap_count >= 3) {                 /* aggressive: engage */
        t->startled = true; t->startle_x = x; t->startle_y = y; t->startle_cooldown = STARTLE_COOLDOWN;
        for (int i = 0; i < t->n_fish; i++) {
            fish_t *f = &t->fish[i];
            if (tank_dist(f->x, f->y, x, y) < STARTLE_RADIUS) {
                f->stress = fminf(10, f->stress + 2.5f);
                f->trust = fmaxf(0, f->trust - 0.4f);
            }
        }
    }
}

/* per-frame bookkeeping for the touch state machine */
static void touch_tick(tank_t *t, float dt) {
    t->tap_burst_t += dt;
    if (!t->startled && t->tap_count == 2 && t->tap_burst_t > TAP_WINDOW) {
        tank_toggle_light(t); t->tap_count = 0;     /* double-tap, then pause */
    }
    if (t->tap_count >= 3 && t->tap_burst_t > TAP_WINDOW) t->tap_count = 0;
    if (t->startled) {
        t->startle_cooldown -= dt;
        if (t->startle_cooldown <= 0) { t->startled = false; t->tap_count = 0; }
    }
    if (t->hold_active) {                           /* calm presence earns trust */
        t->hold_time += dt;
        for (int i = 0; i < t->n_fish; i++) {
            fish_t *f = &t->fish[i];
            float d = tank_dist(f->x, f->y, t->hold_x, t->hold_y);
            if (d < HOLD_RADIUS) f->trust = fminf(10, f->trust + dt * 0.02f);
            /* a fish that comes all the way in and stays = a hold-approach */
            if (d < 30 && t->hold_time > 1.5f && !t->hold_approached) {
                t->hold_approached = true; t->hold_approaches++;
                f->ms_bits |= MS_FIRST_HOLD_APPROACH;
            }
        }
    } else { t->hold_time = 0; t->hold_approached = false; }
    if (t->greet_timer > 0) t->greet_timer -= dt;
}

void tank_toggle_light(tank_t *t) {
    /* first toggle takes over from the auto cycle at the current state */
    if (!t->light_override) { t->light_override = true; t->light_on = t->night; }
    else t->light_on = !t->light_on;
}

void tank_light_auto(tank_t *t) { t->light_override = false; }

void tank_scatter_food(tank_t *t, int n) {
    for (int i = 0; i < MAX_FOOD && n > 0; i++) {
        if (t->food[i].alive) continue;
        t->food[i].alive = true; t->food[i].from_player = false;
        t->food[i].x = tank_randf(t, 25, TANK_W - 25);
        t->food[i].y = tank_randf(t, 6, 20);
        t->food[i].age = 0;
        n--;
    }
}

void tank_start_shadow(tank_t *t) {
    shadow_t *s = &t->shadow;
    s->active = true;
    s->size  = tank_randf(t, 77, 127);
    s->speed = tank_randf(t, 34, 65);
    s->ttl   = tank_randf(t, 8, 15);
    int side = (int)tank_randf(t, 0, 3.999f);
    if (side == 0) { s->x = -100; s->y = tank_randf(t, 40, TANK_H * 0.6f); s->heading = tank_randf(t, -0.16f, 0.26f); }
    if (side == 1) { s->x = TANK_W + 100; s->y = tank_randf(t, 40, TANK_H * 0.6f); s->heading = 3.14159f + tank_randf(t, -0.26f, 0.16f); }
    if (side == 2) { s->x = tank_randf(t, 50, TANK_W - 50); s->y = -80; s->heading = 1.5708f + tank_randf(t, -0.32f, 0.32f); }
    if (side == 3) { s->x = tank_randf(t, 50, TANK_W - 50); s->y = TANK_H + 80; s->heading = -1.5708f + tank_randf(t, -0.32f, 0.32f); }
}

/* ---- goal → target point + cruise speed (prototype targetForGoal, x0.55) ---- */
typedef struct { float x, y, speed; bool valid; } target_t;

/* `goal` is normally f->goal.id; the hesitation glance asks for the runner-up's
 * target, in which case nothing is mutated (no dart burst is started). */
static target_t target_for_goal(tank_t *t, int idx, goal_id_t goal, bool glance) {
    fish_t *f = &t->fish[idx];
    float tm = t->clock;
    target_t tg = {
        f->x + cosf(f->heading + sinf(f->wander) * 0.8f) * 50,
        f->y + sinf(f->heading + cosf(f->wander * 0.7f) * 0.45f) * 39,
        lerpf(12, 23, f->bold) * (1 - f->lazy * 0.35f), true,
    };
    switch (goal) {
    case GOAL_SEEK_FOOD: {
        float d; int i = tank_nearest_food(t, f, &d);
        if (i >= 0) {
            /* hunger governs pursuit aggression regardless of which brain set
             * the goal: a peckish fish saunters (~0.75x), a starving one
             * charges (~1.3x) and barely brakes on approach */
            float h = clampf(f->hunger / 10.0f, 0, 1);
            float slow = clampf(d / 60, 0.4f + h * 0.35f, 1);
            tg.x = t->food[i].x; tg.y = t->food[i].y;
            tg.speed = lerpf(29, 62, f->bold) * slow * (0.75f + h * 0.55f);
            return tg;   /* skip the margin clamp: pellets rest at the floor */
        }
        tg.valid = false;
        break;
    }
    case GOAL_FLEE_SHADOW: {
        float sx = t->shadow.active ? t->shadow.x : TANK_W * 0.5f;
        float sy = t->shadow.active ? t->shadow.y : -60;
        float away = atan2f(f->y - sy, f->x - sx);
        tg.x = f->x + cosf(away) * 105; tg.y = f->y + sinf(away) * 83;
        tg.speed = lerpf(51, 83, f->bold);
        break;
    }
    case GOAL_VISIT_BUBBLES:
        tg.x = t->bubble_x + sinf(tm * 1.2f + f->wander) * 17;
        tg.y = t->bubble_y - 74 + cosf(tm * 0.8f + f->wander) * 28;
        tg.speed = 23;
        break;
    case GOAL_FOLLOW_FRIEND: {
        float d; int i = tank_nearest_friend(t, idx, &d);
        if (i >= 0) {
            const fish_t *fr = &t->fish[i];
            float ang = atan2f(f->y - fr->y, f->x - fr->x) + sinf(tm + f->wander) * 0.35f;
            tg.x = fr->x + cosf(ang) * 34; tg.y = fr->y + sinf(ang) * 22;
            tg.speed = clampf(d - 25, 10, 42);
        } else tg.valid = false;
        break;
    }
    case GOAL_REST:
        tg.x = t->reef_x + 13 + f->rest_dx;
        tg.y = TANK_H - 50 + f->rest_dy;
        tg.speed = 6 + f->bold * 4;
        break;
    case GOAL_DART_PLAY:
        if (glance) { tg.valid = false; break; }
        if (f->dart_timer <= 0) {
            f->dart_x = tank_randf(t, 38, TANK_W - 38);
            f->dart_y = tank_randf(t, 40, TANK_H - 64);
            f->dart_timer = tank_randf(t, 0.8f, 1.5f);
        }
        tg.x = f->dart_x; tg.y = f->dart_y;
        tg.speed = lerpf(65, 95, f->bold);
        break;
    case GOAL_INSPECT_REEF:
        tg.x = t->reef_x + sinf(tm * 0.65f + f->wander) * 39;
        tg.y = t->reef_y - 35 + cosf(tm * 0.8f + f->wander) * 15;
        tg.speed = 15 + f->curiosity * 1.7f;
        break;
    default: if (glance) tg.valid = false; break; /* EXPLORE keeps the wander target */
    }
    float m = 23;
    tg.x = clampf(tg.x, m, TANK_W - m);
    tg.y = clampf(tg.y, m + 9, TANK_H - m);
    return tg;
}

static float wall_avoidance(const fish_t *f, bool *hit) {
    const float m = 34;
    float vx = 0, vy = 0;
    if (f->x < m)          vx += 1 - f->x / m;
    if (f->x > TANK_W - m) vx -= 1 - (TANK_W - f->x) / m;
    if (f->y < m + 6)      vy += 1 - (f->y - 6) / m;
    if (f->y > TANK_H - m) vy -= 1 - (TANK_H - f->y) / m;
    *hit = (vx != 0 || vy != 0);
    return *hit ? atan2f(vy, vx) : 0;
}

static void eat_nearby_food(tank_t *t, fish_t *f) {
    for (int i = 0; i < MAX_FOOD; i++) {
        if (!t->food[i].alive) continue;
        if (tank_dist(f->x, f->y, t->food[i].x, t->food[i].y) < 12 * f->size + 4) {
            t->food[i].alive = false;
            f->hunger = clampf(f->hunger - 4.3f, 0, 10);
            f->energy = clampf(f->energy + 1.0f, 0, 10);
            f->curiosity = clampf(f->curiosity + 0.8f, 0, 10);
            f->eaten++;
            if (t->food[i].from_player) { f->eaten_player++; f->ms_bits |= MS_FIRST_MEAL_FROM_YOU; }
            /* post-meal reflex from the prototype */
            if (f->hunger < 2.2f && f->goal.id == GOAL_SEEK_FOOD)
                f->goal.id = (xr(t) & 1) ? GOAL_VISIT_BUBBLES : GOAL_EXPLORE;
        }
    }
}

static void update_fish(tank_t *t, int idx, float dt) {
    fish_t *f = &t->fish[idx];
    /* drives (prototype rates, speed rescaled by the same 0.55) */
    f->hunger    = clampf(f->hunger + dt * (0.15f + f->bold * 0.12f +
                          (f->goal.id == GOAL_DART_PLAY ? 0.18f : 0)), 0, 10);
    f->curiosity = clampf(f->curiosity + dt * (f->goal.id == GOAL_EXPLORE ? 0.08f : -0.025f), 0, 10);
    f->stress    = clampf(f->stress - dt * (f->goal.id == GOAL_REST ? 0.48f : 0.18f), 0, 10);
    f->energy    = clampf(f->energy + dt * (f->goal.id == GOAL_REST ? 0.55f
                          : -0.055f - f->speed / 950.0f), 0, 10);
    if (t->shadow.active) {
        float sd = tank_dist(f->x, f->y, t->shadow.x, t->shadow.y);
        if (sd < 115) f->stress = clampf(f->stress + dt * 1.7f * (1 - sd / 126), 0, 10);
    }

    f->wander += dt * (0.65f + f->curiosity * 0.04f) + sinf(t->clock + f->x * 0.01f) * dt * 0.12f;
    if (f->dart_timer > 0) f->dart_timer -= dt;
    f->goal_age += dt; f->ask_age += dt;

    target_t tg = target_for_goal(t, idx, f->goal.id, false);
    /* fish prefer shallow climb/dive angles while cruising; full vertical
     * agility stays available for urgent goals */
    bool agile = f->goal.id == GOAL_FLEE_SHADOW || f->goal.id == GOAL_DART_PLAY;
    float desired = atan2f((tg.y - f->y) * (agile ? 1.0f : 0.72f), tg.x - f->x);
    /* final food approach: let the fish dip to the floor for the pellet
     * instead of hovering above it on wall-avoidance (the "staring" bug) */
    bool final_approach = f->goal.id == GOAL_SEEK_FOOD &&
                          tank_dist(f->x, f->y, tg.x, tg.y) < 40;
    bool hit; float push = wall_avoidance(f, &hit);
    if (hit && !final_approach)
        desired = norm_ang(desired + norm_ang(push - desired) * 0.62f);

    /* hesitation: a low-confidence decision is shown, not hidden - the fish
     * hovers and glances between its new target and the runner-up's for a
     * moment before committing. Never for flight or high urgency. */
    float hes_speed = -1;
    if (f->hesitate > 0) {
        f->hesitate -= dt;
        target_t alt = f->goal.runner_up < GOAL_COUNT
                     ? target_for_goal(t, idx, f->goal.runner_up, true) : (target_t){0, 0, 0, false};
        float phase = sinf(t->clock * 5.0f + f->wander);
        if (alt.valid && phase < 0) desired = atan2f((alt.y - f->y) * 0.72f, alt.x - f->x);
        hes_speed = 5;
    }

    /* touch: spooked fish bolt from the tap site; trusting fish drift to a
     * resting finger (reflex-layer, independent of the advisor's goal) */
    float touch_speed = -1;
    if (t->startled && f->goal.id != GOAL_FLEE_SHADOW) {
        float d = tank_dist(f->x, f->y, t->startle_x, t->startle_y);
        if (d < STARTLE_RADIUS * 1.6f) {
            float away = atan2f(f->y - t->startle_y, f->x - t->startle_x);
            desired = norm_ang(desired + norm_ang(away - desired) * 0.85f);
            touch_speed = lerpf(55, 90, f->bold);
        }
    } else if (t->hold_active && f->goal.id != GOAL_FLEE_SHADOW && f->trust >= 4.0f) {
        float d = tank_dist(f->x, f->y, t->hold_x, t->hold_y);
        if (d < HOLD_RADIUS) {
            float w = (f->trust - 4.0f) / 6.0f;          /* 0..1 with trust */
            float to = atan2f(t->hold_y - f->y, t->hold_x - f->x);
            if (d > 28) desired = norm_ang(desired + norm_ang(to - desired) * (0.5f + 0.45f * w));
            touch_speed = d > 28 ? lerpf(14, 30, w) : 4;  /* arrive and hover */
        }
    } else if (t->greet_timer > 0 && f->trust >= 7.0f && f->goal.id != GOAL_FLEE_SHADOW &&
               f->goal.id != GOAL_SEEK_FOOD) {
        /* light-on greeting: trusting fish come up front to see who's there */
        float gx = TANK_W * 0.5f + (idx - t->n_fish * 0.5f) * 34, gy = 70;
        float d = tank_dist(f->x, f->y, gx, gy);
        if (d > 24) {
            float to = atan2f(gy - f->y, gx - f->x);
            desired = norm_ang(desired + norm_ang(to - desired) * 0.7f);
            touch_speed = 26;
        } else touch_speed = 5;
    }

    /* separation */
    for (int i = 0; i < t->n_fish; i++) {
        if (i == idx) continue;
        if (tank_dist(f->x, f->y, t->fish[i].x, t->fish[i].y) < 16 * f->size) {
            float away = atan2f(f->y - t->fish[i].y, f->x - t->fish[i].x);
            desired = norm_ang(desired + norm_ang(away - desired) * 0.45f);
        }
    }

    /* urgency scales cruise speed a touch (0..9 → 0.8..1.25) */
    float ugain = 0.8f + f->goal.urgency * 0.05f;
    float turn = clampf(norm_ang(desired - f->heading), -f->turn_rate * dt, f->turn_rate * dt);
    f->heading = norm_ang(f->heading + turn);
    float want = touch_speed >= 0 ? touch_speed : hes_speed >= 0 ? hes_speed : tg.speed * ugain;
    f->target_speed = want * (f->energy < 1.2f ? 0.45f : 1);
    f->speed = lerpf(f->speed, f->target_speed, clampf(dt * 2.6f, 0, 1));
    f->x += cosf(f->heading) * f->speed * dt;
    f->y += sinf(f->heading) * f->speed * dt + sinf(t->clock * 1.4f + f->wander) * dt * 1.7f;

    /* hard bounds */
    float m = 15 * f->size;
    if (f->x < m)          { f->x = m;          f->heading = norm_ang(3.14159f - f->heading); }
    if (f->x > TANK_W - m) { f->x = TANK_W - m; f->heading = norm_ang(3.14159f - f->heading); }
    if (f->y < m)          { f->y = m;          f->heading = -f->heading; }
    if (f->y > TANK_H - m) { f->y = TANK_H - m; f->heading = -f->heading; }

    eat_nearby_food(t, f);
}

/* Coarse state signature (the browser prototype's "blunt" gate): banded
 * drives plus bucketed sightings. Excludes clock bearings and exact drive
 * digits, which only steer - a change here is a reason to re-decide. */
static int band3(float v) { return v < 3.5f ? 0 : v < 7 ? 1 : 2; }
static int dbucket(float d) { return d < 70 ? 0 : d < 180 ? 1 : d < 380 ? 2 : 3; }
static uint32_t state_signature(const tank_t *t, int idx) {
    const fish_t *f = &t->fish[idx];
    float fd = 1e9f; tank_nearest_food(t, f, &fd);
    float sd = t->shadow.active ? tank_dist(f->x, f->y, t->shadow.x, t->shadow.y) : 1e9f;
    int wall = f->x < 70 || f->x > TANK_W - 70 || f->y < 70 || f->y > TANK_H - 70;
    return (uint32_t)band3(f->hunger) | (uint32_t)band3(f->energy) << 2 | (uint32_t)band3(f->stress) << 4
         | (uint32_t)dbucket(fd) << 6 | (uint32_t)dbucket(sd) << 8 | (uint32_t)wall << 10
         | (uint32_t)t->night << 11;
}

void tank_tick(tank_t *t, float dt, advisor_fn advise) {
    t->clock += dt;
    /* 240s day/night cycle: 160s day, 80s night; a user light override wins */
    t->day_phase = fmodf(t->clock, 240.0f) / 240.0f;
    t->night = t->light_override ? !t->light_on : t->day_phase > 0.6667f;

    touch_tick(t, dt);
    bool hold_now = t->hold_active;   /* consumed this frame; platform re-asserts */
    t->hold_active = false;
    (void)hold_now;

    /* food sinks, settles, decays */
    int live_food = 0;
    for (int i = 0; i < MAX_FOOD; i++) {
        food_t *p = &t->food[i];
        if (!p->alive) continue;
        live_food++;
        p->age += dt;
        if (p->y < TANK_H - 14) {
            p->y += 8 * dt;
            p->x += sinf(p->age * 1.5f) * dt * 2;
        }
        if (p->age > 45) p->alive = false;
    }
    /* the tank's own trickle keeps fish alive when nobody is home (never
     * ruined by absence); the keeper's pellets are what progression counts */
    if (live_food < 2 && tank_randf(t, 0, 1) < 0.001f * t->n_fish) tank_scatter_food(t, 1);

    /* bubbles rise */
    for (int i = 0; i < MAX_BUBBLE; i++) {
        bubble_t *b = &t->bubble[i];
        b->wobble += dt * 2.5f;
        b->y -= b->vy * dt;
        b->x += sinf(b->wobble) * dt * (b->column ? 9 : 4);
        if (b->y < -6) {
            b->y = TANK_H + tank_randf(t, 4, 24);
            b->x = b->column ? t->bubble_x + tank_randf(t, -10, 10)
                             : tank_randf(t, 12, TANK_W - 12);
        }
    }

    /* shadow roams / respawns */
    shadow_t *s = &t->shadow;
    if (s->active) {
        s->ttl -= dt;
        s->x += cosf(s->heading) * s->speed * dt;
        s->y += sinf(s->heading) * s->speed * dt;
        if (s->ttl <= 0 || s->x < -150 || s->x > TANK_W + 150 || s->y < -130 || s->y > TANK_H + 130) {
            s->active = false;
            s->cool = tank_randf(t, 24, 48);
        }
    } else {
        s->cool -= dt;
        if (s->cool <= 0) tank_start_shadow(t);
    }

    /* advisor: polled every frame (async decisions land the moment they're
     * ready). A (re)decision is REQUESTED need-based, not on a fixed cadence:
     * the coarse signature changed and ADVISOR_MIN_INTERVAL passed, or the
     * idle ceiling hit, or something urgent (shadow closing, starving with
     * food in view). Effective cadence therefore scales with how much is
     * happening, not with how many fish live here. The start index rotates
     * so no slot is structurally favoured when several fish ask at once. */
    if (advise && t->n_fish > 0) {
        for (int k = 0; k < t->n_fish; k++) {
            int i = (t->ask_rr + k) % t->n_fish;
            fish_t *f = &t->fish[i];
            uint32_t sig = state_signature(t, i);
            bool urgent = (s->active && f->goal.id != GOAL_FLEE_SHADOW &&
                           tank_dist(f->x, f->y, s->x, s->y) < 90 && f->goal_age > 0.5f)
                          /* prototype's urgent path: starving with food in view
                           * gets asked NOW instead of waiting its turn */
                          || (f->hunger > 8.0f && f->goal.id != GOAL_SEEK_FOOD &&
                              f->goal_age > 0.8f && tank_nearest_food(t, f, 0) >= 0);
            bool changed = sig != f->sig && f->ask_age > ADVISOR_MIN_INTERVAL;
            bool idle = f->ask_age > ADVISOR_IDLE_CEILING;
            bool want = changed || idle || (urgent && f->ask_age > 0.5f);
            goal_t g = advise(t, i, want);
            if (want) {            /* async advisors queue it; rules answer now */
                f->sig = sig; f->ask_age = 0; t->advisor_asks++;
                t->ask_rr = (i + 1) % t->n_fish;
            }
            if (g.id < GOAL_COUNT && g.id != f->goal.id) {
                f->goal = g;
                f->goal_age = 0;
                /* visible deliberation, scaled by how torn the advisor was */
                bool calm = g.id != GOAL_FLEE_SHADOW && g.urgency < 8 && g.confidence < 0.6f;
                f->hesitate = calm ? (0.6f - g.confidence) * 2.5f : 0;
            } else if (g.id == f->goal.id) {
                f->goal.urgency = g.urgency;
                f->goal.confidence = g.confidence; f->goal.runner_up = g.runner_up;
            }
        }
    }

    /* Starvation instrument (was a TEMPORARY survival-reflex override; retired
     * 2026-08-21 after the retrained model chose seek_food on 92% of starving
     * states with defensible misses - per Strato: the LLM owns decisions).
     * Counts moments a starving fish ignores available food for >4s. Pure
     * diagnostic, never changes a goal. */
    for (int i = 0; i < t->n_fish; i++) {
        fish_t *f = &t->fish[i];
        if (f->hunger > 8.5f && f->goal.id != GOAL_SEEK_FOOD &&
            f->goal.id != GOAL_FLEE_SHADOW && f->goal_age > 4.0f &&
            !f->starve_flagged && tank_nearest_food(t, f, 0) >= 0) {
            f->starve_flagged = true;          /* count once per episode */
            tank_reflex_overrides++;
        }
        if (f->goal.id == GOAL_SEEK_FOOD || f->hunger < 8.0f) f->starve_flagged = false;
    }

    for (int i = 0; i < t->n_fish; i++) update_fish(t, i, dt);
}
