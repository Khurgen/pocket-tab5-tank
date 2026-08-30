/* tank.h — platform-agnostic reflex layer for the pocket fish tank.
 *
 * Ported from the browser prototype (llm-fishtank-v0_2.html); same fish,
 * same goals, same drive dynamics, rescaled to the 448x368 landscape tank.
 * World geometry (bubble column, reef, zones) matches model/gen_traces.py so
 * the advisor model sees the world it was trained on.
 *
 * Population (docs/progression-next.md): the tank holds up to N_FISH_MAX fish
 * but only fish[0..n_fish-1] are alive/active. A new tank starts with two
 * adults of contrasting personality; the rest arrive later as fry (the
 * progression layer decides when). Every loop in this file, the renderer and
 * the advisors iterates n_fish, never N_FISH_MAX.
 *
 * No I/O, no floats-to-strings, no OS calls: this file and tank.c compile
 * unchanged for the LVGL PC sim and the ESP32-S3 firmware.
 */
#ifndef POCKET_TANK_TANK_H
#define POCKET_TANK_TANK_H

#include <stdbool.h>
#include <stdint.h>

#define TANK_W 448
#define TANK_H 368

#define N_FISH_MAX 6            /* array bound; the live count is tank_t.n_fish */
#define N_FISH_START 2          /* a new tank: two contrasting adults */
#define N_TRAINED_NAMES 4       /* name tokens the v2 model was trained on */
#define MAX_FOOD   8
#define MAX_BUBBLE 24

typedef enum {
    GOAL_SEEK_FOOD, GOAL_FLEE_SHADOW, GOAL_VISIT_BUBBLES, GOAL_FOLLOW_FRIEND,
    GOAL_EXPLORE, GOAL_REST, GOAL_DART_PLAY, GOAL_INSPECT_REEF,
    GOAL_COUNT
} goal_id_t;

extern const char *const GOAL_NAMES[GOAL_COUNT];   /* schema.md lowercase names */

typedef enum { STAGE_FRY, STAGE_JUV, STAGE_ADULT, STAGE_ELDER } stage_t;
extern const char *const STAGE_NAMES[4];           /* schema.md v2 stage tokens */
extern const char *const TRAINED_NAMES[N_TRAINED_NAMES]; /* mira bolt kelp nori */

typedef struct {
    goal_id_t id;
    float     urgency;      /* 0..9, scales speed/turn gain */
    /* distribution layer (LLM advisor only; rules leave confidence = 1):
     * confidence = probability the advisor put on this goal; runner_up = the
     * second choice. The reflex layer renders low confidence as a visible
     * hesitation (pause + glance at the runner-up's target) - the decision is
     * never altered, only its certainty is shown. */
    float     confidence;
    goal_id_t runner_up;    /* GOAL_COUNT when unknown */
} goal_t;

/* per-fish milestone bits (progression.c detects, render reads) */
enum {
    MS_ARRIVED = 1u << 0,  MS_FIRST_MEAL_FROM_YOU = 1u << 1, MS_FIRST_HOLD_APPROACH = 1u << 2,
    MS_FIRST_DART = 1u << 3, MS_FIRST_BUBBLES = 1u << 4, MS_FIRST_REEF = 1u << 5,
    MS_FIRST_SHADOW_SURVIVED = 1u << 6, MS_FIRST_SHRUG = 1u << 7, MS_FIRST_FOLLOW = 1u << 8,
    MS_REACHED_JUV = 1u << 9, MS_REACHED_ADULT = 1u << 10, MS_REACHED_ELDER = 1u << 11,
    MS_FISH_COUNT = 12
};
/* tank-level milestone bits */
enum {
    TMS_PAIR = 1u << 0, TMS_TRIO = 1u << 1, TMS_QUARTET = 1u << 2, TMS_QUINTET = 1u << 3,
    TMS_SEXTET = 1u << 4, TMS_FIRST_QUIET_NIGHT = 1u << 5, TMS_FIRST_PLAY_SESSION = 1u << 6,
    TMS_CHANGED_SOMEONE = 1u << 7, TMS_FIRST_FEEDING = 1u << 8,
    TMS_COUNT = 9
};

typedef struct {
    const char *name;       /* display name (roster preset) */
    const char *model_name; /* one of TRAINED_NAMES: the token the v2 model sees */
    int    preset;          /* roster index (colors, base size, temperament) */
    float x, y;
    float heading;          /* radians */
    float speed, target_speed;
    float wander;           /* wander phase */
    float size;             /* ~1.0 */
    float base_size;        /* preset size; progression scales by stage/meals */
    float turn_rate;        /* rad/s */
    /* drives, 0..10 as in the prototype (schema encoding clamps to 0..9) */
    float hunger, energy, stress, curiosity;
    /* personality 0..1 (schema v2 encodes bold/sociable as 0-9) */
    float sociable, bold, lazy;
    float bold0, sociable0; /* values at creation: drift is measured from here */
    stage_t stage;
    goal_t goal;
    float  goal_age;        /* seconds since last goal change */
    float  ask_age;         /* seconds since the advisor was last asked about it */
    float  dart_timer;      /* DART_PLAY burst countdown */
    float  dart_x, dart_y;
    float  hesitate;        /* seconds of visible deliberation left (0 = none) */
    int    eaten;
    int    eaten_player;    /* pellets that came from the keeper's hand */
    bool   starve_flagged;  /* diagnostic: starving-ignored-food episode counted */
    float  trust;           /* 0..10: gates approach-to-finger; drifts with treatment */
    float  rest_dx, rest_dy;/* this fish's own spot by the reef (individuation) */
    uint32_t sig;           /* coarse state signature at the last advisor ask */
    uint32_t ms_bits;       /* MS_* milestones reached */
    /* colors as 0xRRGGBB, used by render only */
    uint32_t color, fin, accent;
} fish_t;

typedef struct { float x, y, age; bool alive, from_player; } food_t;
typedef struct { float x, y, vy, wobble; bool column; } bubble_t;

typedef struct {
    bool  active;
    float x, y, heading, speed, size, ttl, cool;
} shadow_t;

typedef struct tank {
    fish_t   fish[N_FISH_MAX];
    int      n_fish;               /* live fish = fish[0..n_fish-1] */
    food_t   food[MAX_FOOD];
    bubble_t bubble[MAX_BUBBLE];
    shadow_t shadow;
    float    bubble_x, bubble_y;   /* bubble column anchor */
    float    reef_x, reef_y;
    float    clock;                /* seconds since start */
    float    day_phase;            /* 0..1 within the day/night cycle */
    bool     night;
    bool     light_override;       /* user took manual control of the light */
    bool     light_on;
    /* touch / tap interaction (docs/progression.md). Reflex-layer only: the
     * advisor never sees taps directly, only their effect on stress. */
    bool     hold_active;          /* finger held on the glass this frame */
    float    hold_x, hold_y;
    float    hold_time;            /* seconds the current hold has lasted */
    bool     hold_approached;      /* a fish already came in during this hold */
    int      tap_count;            /* taps in the current burst */
    float    tap_burst_t;          /* seconds since last tap */
    float    tap_x, tap_y;
    bool     startled;             /* aggressive-tap flee mode engaged */
    float    startle_x, startle_y;
    float    startle_cooldown;     /* seconds of calm needed to disengage */
    /* keeper habits the tank remembers (persisted by progression.c) */
    float    feed_spot_x;          /* where the keeper usually feeds (EMA); <0 = unknown */
    int      player_feedings;      /* feed gestures so far */
    int      hold_approaches;      /* calm holds that drew a fish all the way in */
    float    greet_timer;          /* light-on greeting: trusting fish come up front */
    bool     ravenous;             /* starving tank, empty water: fish beg at the
                                    * surface, trickle holds off (progression.c
                                    * owns entry/exit; tank.c renders the wait) */
    uint32_t tank_ms_bits;         /* TMS_* milestones reached */
    /* advisor scheduling (need-based, see tank_tick) */
    int      ask_rr;               /* rotating start index for fairness */
    uint32_t advisor_asks;         /* diagnostic: requests issued */
    uint32_t rng;                  /* xorshift state, deterministic */
} tank_t;

/* Advisor interface (Track 1 model, rule stub, or firmware LLM core).
 * Called EVERY frame for every live fish. `request` is true when the tank
 * wants a (re)decision: the fish's coarse state signature changed and the
 * minimum interval passed, it hit the idle ceiling, or something urgent
 * happened (shadow closing in, starving with food in view). When false it's
 * a poll: an async advisor returns a completed decision the moment it's
 * ready; otherwise return the fish's current goal unchanged. */
typedef goal_t (*advisor_fn)(const tank_t *t, int fish_idx, bool request);

#define ADVISOR_MIN_INTERVAL 2.0f   /* s between asks for one fish (signature changed) */
#define ADVISOR_IDLE_CEILING 9.0f   /* s: re-ask even if nothing changed */

void  tank_init(tank_t *t, uint32_t seed);
/* Population. tank_init leaves the tank empty (n_fish = 0); the progression
 * layer either restores a save or calls tank_new_population for a fresh tank:
 * two random roster presets, personalities rolled with a guaranteed contrast,
 * adults. tank_add_fish appends the next unused preset as a fry whose
 * bold/social are inherited from two live fish (+ noise); returns the index or
 * -1 if the tank is full. */
void  tank_new_population(tank_t *t);
int   tank_add_fish(tank_t *t, int parent_a, int parent_b);
/* (re)build slot from a roster preset: used by persistence to restore a fish */
void  tank_make_fish(tank_t *t, int slot, int preset, float sociable, float bold, stage_t stage);
void  tank_tick(tank_t *t, float dt, advisor_fn advise);
/* Sleep metabolism (device drowse mode: screen dark, fish asleep). Advances
 * ONLY slow physiology - hunger up, energy recovered, stress gone - at
 * real-hours scale; no movement, no goals, no eating, no trickle. Sleeping
 * fish make no decisions, so the advisor contract is untouched. Safe to call
 * with hours at a time. */
void  tank_tick_sleep(tank_t *t, float seconds);
/* Tank light: overrides the day/night cycle (the device maps a touch gesture
 * to this). tank_light_auto returns to the automatic cycle. */
void  tank_toggle_light(tank_t *t);
void  tank_light_auto(tank_t *t);

/* Touch input (platform feeds these; sim = mouse, device = FT3168):
 *  tank_touch_hold: call EVERY FRAME while a finger rests on the glass at x,y.
 *    High-trust fish drift over to investigate; low-trust fish keep away.
 *  tank_touch_tap:  call once per tap. A tap on the water surface (y below
 *    FEED_ZONE_Y) is a FEED gesture: pellets drop there, nothing else happens.
 *    Elsewhere: 2 quick taps then a pause toggles the light; 3+ quick taps =
 *    aggressive -> nearby fish flee the spot and stay spooked while taps
 *    continue (cooldown resets on each tap); after the cooldown, single taps
 *    are harmless again and it takes 3 quick taps to re-trigger. Spooking
 *    costs trust; calm holds earn it.
 *  tank_feed: the keeper drops n pellets at x (surface). Player feeding is what
 *    progression counts; the tank's own trickle feed is not. */
#define FEED_ZONE_Y 26.0f
void  tank_touch_hold(tank_t *t, float x, float y);
void  tank_touch_tap(tank_t *t, float x, float y);
void  tank_feed(tank_t *t, float x, int n);

/* Diagnostic: episodes where a starving fish ignored available food >4s.
 * Never alters behavior - the advisor owns every decision. */
extern int tank_reflex_overrides;
void  tank_scatter_food(tank_t *t, int n);      /* the tank's own trickle (random x) */
void  tank_start_shadow(tank_t *t);

/* helpers shared with advisor/render/progression */
float tank_dist(float ax, float ay, float bx, float by);
int   tank_nearest_food(const tank_t *t, const fish_t *f, float *dist_out);
int   tank_nearest_friend(const tank_t *t, int fish_idx, float *dist_out);
float tank_randf(tank_t *t, float lo, float hi);
/* roster preset count (6) and a preset's display name, for UI */
int   tank_roster_count(void);
const char *tank_roster_name(int preset);

#endif
