/* main.c — PC simulator entry: LVGL v9 + SDL window, 448x368 to match the
 * ESP32 AMOLED (landscape). This is the only platform-specific file; tank.c,
 * advisor.c and render.c compile unchanged for firmware.
 *
 *   ./fishsim              run the tank (keys: F feed at the mouse x, S shadow,
 *                          N light, L brain, U overlays, M milestones view,
 *                          R force an arrival (debug), A auto-light, Q quit;
 *                          click fish = stats; tap the water surface = feed;
 *                          drag down from the top = feed; hold mouse = finger
 *                          on glass; 3 quick taps = startle; 2 taps = light)
 *   ./fishsim --fresh      ignore the save (new tank: random pair)
 *   ./fishsim --fast N     tended time runs N x faster (stages, drift)
 *   ./fishsim --greedy     greedy decoding instead of sampling
 *   ./fishsim --selftest   headless reflex-layer check, no window
 *   ./fishsim --selftest-llm [min]   headless LLM path (real-time if min > 0)
 *   ./fishsim --selftest-pop         headless population/arrival/save check
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include "tank.h"
#include "advisor.h"
#include "advisor_core.h"
#include "render.h"
#include "progression.h"

static tank_t tank;

static void print_roster(const tank_t *t) {
    printf("tank: %d fish\n", t->n_fish);
    for (int i = 0; i < t->n_fish; i++)
        printf("  %-5s (model token %-4s) %s bold %.2f social %.2f trust %.1f\n",
               t->fish[i].name, t->fish[i].model_name, STAGE_NAMES[t->fish[i].stage],
               t->fish[i].bold, t->fish[i].sociable, t->fish[i].trust);
}

/* ---------- headless selftest ---------- */
static int selftest(void) {
    tank_init(&tank, 1234);
    tank_new_population(&tank);
    print_roster(&tank);
    if (tank.n_fish != 2 || fabsf(tank.fish[0].bold - tank.fish[1].bold) < 0.45f) {
        printf("FAIL: starting pair not contrasting\n"); return 1;
    }
    /* grow the population to the max through the tank API */
    while (tank_add_fish(&tank, 0, 1) >= 0) {}
    if (tank.n_fish != N_FISH_MAX) { printf("FAIL: population %d != %d\n", tank.n_fish, N_FISH_MAX); return 1; }
    int goal_seen[GOAL_COUNT] = {0};
    for (int i = 0; i < 7200; i++) {              /* 2 simulated minutes */
        tank_tick(&tank, 1.0f / 60.0f, advisor_rules);
        if (i == 600) tank_feed(&tank, 200, 3);
        if (i == 1200) tank_touch_tap(&tank, 300, 10);   /* surface tap = feed */
        for (int fi = 0; fi < tank.n_fish; fi++) {
            const fish_t *f = &tank.fish[fi];
            if (!(f->x >= 0 && f->x <= TANK_W && f->y >= 0 && f->y <= TANK_H) ||
                f->x != f->x || f->y != f->y) {   /* NaN check */
                printf("FAIL: %s out of bounds at tick %d (%.1f,%.1f)\n",
                       f->name, i, f->x, f->y);
                return 1;
            }
            goal_seen[f->goal.id]++;
        }
    }
    static uint16_t fb[TANK_W * TANK_H], scene[TANK_W * TANK_H];
    render_set_scene_cache(scene);
    render_tank(&tank, fb, TANK_W);               /* renderer must not crash */
    render_stats_card(&tank, 0, fb, TANK_W);
    render_milestones(&tank, fb, TANK_W);
    int eaten = 0, distinct = 0;
    for (int i = 0; i < tank.n_fish; i++) eaten += tank.fish[i].eaten;
    printf("selftest: 7200 ticks ok, %u advisor asks, %d player feedings, feed spot %.0f\n",
           tank.advisor_asks, tank.player_feedings, tank.feed_spot_x);
    for (int g = 0; g < GOAL_COUNT; g++) {
        if (goal_seen[g]) distinct++;
        printf("  %-14s %5d fish-ticks\n", GOAL_NAMES[g], goal_seen[g]);
    }
    printf("  pellets eaten: %d, distinct goals used: %d/8\n", eaten, distinct);
    for (int i = 0; i < tank.n_fish; i++)
        printf("  %-5s (%.0f,%.0f) hunger %.1f energy %.1f goal %s\n",
               tank.fish[i].name, tank.fish[i].x, tank.fish[i].y,
               tank.fish[i].hunger, tank.fish[i].energy,
               GOAL_NAMES[tank.fish[i].goal.id]);
    if (tank.player_feedings != 2) { printf("FAIL: feed gestures not counted\n"); return 1; }
    return distinct >= 5 ? 0 : 1;                 /* a live tank uses most goals */
}

/* population + progression + persistence, headless and fast */
static int selftest_pop(void) {
    setenv("POCKET_TANK_SAVE", "/tmp/pocket-tank-selftest.sav", 1);   /* never touch the real save */
    char cmd[600]; snprintf(cmd, sizeof cmd, "rm -f /tmp/pocket-tank-selftest.sav"); (void)system(cmd);
    tank_init(&tank, 99);
    progression_boot(&tank);                      /* no save -> new random pair */
    print_roster(&tank);
    if (tank.n_fish != 2) { printf("FAIL: new tank should start with 2\n"); return 1; }
    progression_time_scale = 600;                 /* 10 minutes of tended time per second */
    int arrivals = 0, last_n = tank.n_fish;
    for (int i = 0; i < 60 * 60 * 6; i++) {       /* 6 sim-minutes */
        tank_tick(&tank, 1.0f / 60.0f, advisor_rules);
        /* an attentive keeper: feeds often, rests a finger by a fish */
        if (i % 300 == 0) tank_feed(&tank, 150 + (i % 900) / 3, 2);
        if (i % 120 < 100) tank_touch_hold(&tank, tank.fish[0].x + 10, tank.fish[0].y);
        progression_tick(&tank, 1.0f / 60.0f);
        if (tank.n_fish != last_n) {
            printf("  t=%.0fs arrival: %s (%s) bold %.2f social %.2f -> %d fish\n", tank.clock,
                   tank.fish[tank.n_fish - 1].name, STAGE_NAMES[tank.fish[tank.n_fish - 1].stage],
                   tank.fish[tank.n_fish - 1].bold, tank.fish[tank.n_fish - 1].sociable, tank.n_fish);
            last_n = tank.n_fish; arrivals++;
        }
    }
    printf("selftest-pop: %d arrivals, %d fish, feedings %d, hold-approaches %d, pending %d\n",
           arrivals, tank.n_fish, tank.player_feedings, tank.hold_approaches, progression_arrival_pending());
    for (int i = 0; i < tank.n_fish; i++)
        printf("  %-5s %s age %.0fh size %.2f ms 0x%03x trust %.1f\n", tank.fish[i].name,
               STAGE_NAMES[tank.fish[i].stage], progression_age_s(&tank, i) / 3600, tank.fish[i].size,
               tank.fish[i].ms_bits, tank.fish[i].trust);
    printf("  tank ms 0x%03x\n", tank.tank_ms_bits);
    if (arrivals < 1) { printf("FAIL: no arrival earned by an attentive keeper\n"); return 1; }
    /* round-trip the save */
    bool pending_at_save = progression_arrival_pending();
    progression_save(&tank);
    tank_t saved = tank;
    tank_init(&tank, 5); progression_boot(&tank);
    /* an arrival earned but not yet shown is delivered at boot (light on) */
    int expect = saved.n_fish + (pending_at_save ? 1 : 0);
    if (tank.n_fish != expect) { printf("FAIL: restore n_fish %d != %d\n", tank.n_fish, expect); return 1; }
    for (int i = 0; i < saved.n_fish; i++)
        if (tank.fish[i].preset != saved.fish[i].preset || tank.fish[i].ms_bits != saved.fish[i].ms_bits ||
            fabsf(tank.fish[i].bold - saved.fish[i].bold) > 1e-4f) {
            printf("FAIL: restore mismatch on fish %d\n", i); return 1;
        }
    printf("  save/restore ok (%d fish, tank ms 0x%03x)\n", tank.n_fish, tank.tank_ms_bits);
    (void)system(cmd);                            /* leave no test save behind */
    return 0;
}

/* ---------- LVGL + SDL ---------- */
#include "lvgl/lvgl.h"
#include <SDL2/SDL.h>

static lv_draw_buf_t draw_buf;
static uint16_t canvas_buf[TANK_W * TANK_H];
static uint16_t scene_buf[TANK_W * TANK_H];
static lv_obj_t *canvas;
static uint32_t last_ms;
static bool llm_available = false;
static bool llm_active = false;
static int  selected_fish = -1;      /* click a fish for its stat card */
static bool ui_visible = true;       /* U toggles all overlays */
static bool milestones_view = false; /* M toggles the milestones screen */

static uint32_t tick_cb(void) { return SDL_GetTicks(); }

/* brain indicator, top-right: teal square = rules, amber = LLM */
static void draw_brain_dot(void) {
    uint16_t col = llm_active ? 0xFDC0 /*amber*/ : 0x3E98 /*teal*/;
    for (int y = 4; y < 10; y++)
        for (int x = TANK_W - 10; x < TANK_W - 4; x++)
            canvas_buf[y * TANK_W + x] = col;
}

static void frame_cb(lv_timer_t *timer) {
    (void)timer;
    uint32_t now = SDL_GetTicks();
    float dt = (now - last_ms) / 1000.0f;
    last_ms = now;
    if (dt > 0.1f) dt = 0.1f;                     /* window drag pause */
    tank_tick(&tank, dt, llm_active ? advisor_llm : advisor_rules);
    progression_tick(&tank, dt);
    if (milestones_view) render_milestones(&tank, canvas_buf, TANK_W);
    else {
        render_tank(&tank, canvas_buf, TANK_W);
        if (ui_visible) {
            draw_brain_dot();
            if (selected_fish >= 0)
                render_stats_card(&tank, selected_fish, canvas_buf, TANK_W);
        }
    }
    lv_obj_invalidate(canvas);
}

/* headless check of the LLM advisor path: encode → infer → goals applied.
 * minutes > 0 runs REAL-TIME pacing for that long (the honest measurement of
 * survival-reflex overrides); minutes == 0 is the fast smoke test. */
static int selftest_llm(int minutes) {
    if (!advisor_llm_init("../model/out/model_q4.bin", "../model/out/tokenizer.bin")) {
        printf("FAIL: model.bin/tokenizer.bin not found under ../model/out/\n");
        return 1;
    }
    tank_init(&tank, 4321);
    tank_new_population(&tank);
    while (tank.n_fish < 4) tank_add_fish(&tank, 0, 1);   /* the 4-fish tank the soak numbers refer to */
    for (int i = 2; i < 4; i++) tank.fish[i].stage = STAGE_ADULT;
    print_roster(&tank);
    printf("warm-up (pages in the mmap'd weights):\n");
    advisor_llm_debug(&tank, 0);
    advisor_llm_debug(&tank, 1);
    int changes = 0, torn = 0;
    int ticks = minutes > 0 ? minutes * 3600 : 3600;
    int delay = minutes > 0 ? 16 : 2;             /* 16ms = real-time 60fps */
    goal_id_t last[N_FISH_MAX];
    for (int i = 0; i < tank.n_fish; i++) last[i] = tank.fish[i].goal.id;
    for (int i = 0; i < ticks; i++) {
        tank_tick(&tank, 1.0f / 60.0f, advisor_llm);
        SDL_Delay(delay);
        if (i % (ticks / 4) == 0) {
            printf("  tick %5d goals:", i);
            for (int fi = 0; fi < tank.n_fish; fi++) printf(" %s", GOAL_NAMES[tank.fish[fi].goal.id]);
            printf(" | asks %u overrides %d\n", tank.advisor_asks, tank_reflex_overrides);
        }
        for (int fi = 0; fi < tank.n_fish; fi++)
            if (tank.fish[fi].goal.id != last[fi]) {
                last[fi] = tank.fish[fi].goal.id;
                changes++;
                if (tank.fish[fi].goal.confidence < 0.6f) torn++;
                if (changes <= 8)
                    printf("  t=%.1fs %-5s -> %s (urgency %.0f, p=%.2f%s)\n",
                           tank.clock, tank.fish[fi].name,
                           GOAL_NAMES[tank.fish[fi].goal.id],
                           tank.fish[fi].goal.urgency, tank.fish[fi].goal.confidence,
                           tank.fish[fi].hesitate > 0 ? ", hesitating" : "");
            }
    }
    printf("selftest-llm: %d goal changes (%d torn), %u asks, %d survival overrides in %d sim-seconds\n",
           changes, torn, tank.advisor_asks, tank_reflex_overrides, ticks / 60);
    return changes >= 4 ? 0 : 1;                  /* a live brain redirects fish */
}

/* --snapshot <prefix> [seconds]: run headless (rules brain, all 6 fish, fast
 * progression) and write <prefix>_tank.ppm, _card.ppm, _milestones.ppm -
 * a look at the renderer without a window (docs, review, CI). */
static void write_ppm(const char *path, const uint16_t *fb) {
    FILE *f = fopen(path, "wb"); if (!f) return;
    fprintf(f, "P6\n%d %d\n255\n", TANK_W, TANK_H);
    for (int i = 0; i < TANK_W * TANK_H; i++) {
        uint16_t p = fb[i];
        unsigned char rgb[3] = { (unsigned char)(((p >> 11) & 31) << 3), (unsigned char)(((p >> 5) & 63) << 2),
                                 (unsigned char)((p & 31) << 3) };
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
}
static int snapshot(const char *prefix, int seconds) {
    tank_init(&tank, 2024);
    tank_new_population(&tank);
    while (tank.n_fish < N_FISH_MAX) tank_add_fish(&tank, 0, 1);
    for (int i = 2; i < tank.n_fish; i++) tank.fish[i].stage = (stage_t)(i % 4);
    tank.fish[0].stage = STAGE_ELDER; tank.fish[0].ms_bits = 0xfff; tank.fish[1].ms_bits = 0x1c7;
    tank.tank_ms_bits = 0x1a7;
    for (int i = 0; i < seconds * 60; i++) {
        tank_tick(&tank, 1.0f / 60.0f, advisor_rules);
        if (i % 400 == 0) tank_feed(&tank, 220, 3);
        if (i == seconds * 30) tank_start_shadow(&tank);
    }
    static uint16_t fb[TANK_W * TANK_H], scene[TANK_W * TANK_H];
    char path[512];
    render_set_scene_cache(scene);
    render_tank(&tank, fb, TANK_W);
    snprintf(path, sizeof path, "%s_tank.ppm", prefix); write_ppm(path, fb);
    render_tank(&tank, fb, TANK_W); render_stats_card(&tank, 0, fb, TANK_W);
    snprintf(path, sizeof path, "%s_card.ppm", prefix); write_ppm(path, fb);
    render_milestones(&tank, fb, TANK_W);
    snprintf(path, sizeof path, "%s_milestones.ppm", prefix); write_ppm(path, fb);
    printf("snapshot: %d fish, wrote %s_{tank,card,milestones}.ppm\n", tank.n_fish, prefix);
    return 0;
}

int main(int argc, char **argv) {
    for (int a = 1; a < argc; a++)
        if (strcmp(argv[a], "--greedy") == 0) advisor_core_sample = false;
    for (int a = 1; a < argc; a++) {                 /* mode flags may sit anywhere */
        if (strcmp(argv[a], "--snapshot") == 0 && a + 1 < argc)
            return snapshot(argv[a + 1], a + 2 < argc ? atoi(argv[a + 2]) : 20);
        if (strcmp(argv[a], "--selftest") == 0) return selftest();
        if (strcmp(argv[a], "--selftest-pop") == 0) return selftest_pop();
        if (strcmp(argv[a], "--selftest-llm") == 0)
            return selftest_llm(a + 1 < argc ? atoi(argv[a + 1]) : 0);
    }

    tank_init(&tank, (uint32_t)SDL_GetTicks() + 7);
    bool fresh = false;
    for (int a = 1; a < argc; a++) {
        if (strcmp(argv[a], "--fresh") == 0) fresh = true;
        if (strcmp(argv[a], "--fast") == 0 && a + 1 < argc) progression_time_scale = (float)atof(argv[++a]);
    }
    if (fresh) {
        char cmd[600]; snprintf(cmd, sizeof cmd, "rm -f '%s/.cache/pocket-tank/tank.sav'", getenv("HOME") ? getenv("HOME") : ".");
        (void)system(cmd);
    }
    progression_boot(&tank);               /* restore, or a new random pair */
    print_roster(&tank);
    for (int a = 1; a < argc; a++)
        if (strcmp(argv[a], "--narrate") == 0) {
            advisor_llm_narrate = true;   /* film the tank + this terminal */
            llm_active = true;
        }
    llm_available = advisor_llm_init("../model/out/model_q4.bin",
                                     "../model/out/tokenizer.bin");
    printf(llm_available
           ? "LLM advisor loaded (press L to toggle rule/LLM brain)\n"
           : "model.bin/tokenizer.bin not found; rule brain only\n");
    lv_init();
    lv_tick_set_cb(tick_cb);
    lv_display_t *disp = lv_sdl_window_create(TANK_W, TANK_H);
    lv_sdl_window_set_title(disp, "pocket-tank sim 448x368");

    lv_draw_buf_init(&draw_buf, TANK_W, TANK_H, LV_COLOR_FORMAT_RGB565,
                     TANK_W * 2, canvas_buf, sizeof(canvas_buf));
    canvas = lv_canvas_create(lv_screen_active());
    lv_canvas_set_draw_buf(canvas, &draw_buf);
    lv_obj_center(canvas);
    render_set_scene_cache(scene_buf);

    last_ms = SDL_GetTicks();
    lv_timer_create(frame_cb, 16, NULL);

    bool fdown = false, sdown = false, ndown = false, ldown = false;
    bool udown = false, mdown = false, mkdown = false, rdown = false;
    uint32_t press_ms = 0; int press_x = 0, press_y = 0;
    while (1) {
        uint32_t wait = lv_timer_handler();
        const Uint8 *k = SDL_GetKeyboardState(NULL);
        int mx, my;
        bool mpress = SDL_GetMouseState(&mx, &my) & SDL_BUTTON(SDL_BUTTON_LEFT);
        uint32_t now_ms = SDL_GetTicks();
        /* mouse -> touch gestures (device: FT3168 does the same job)
         *   press+release < 250 ms, little movement: TAP (on a fish = select its card;
         *                                              on the surface = feed)
         *   drag down >= 40 px starting near the top: FEED at that x
         *   held > 300 ms: HOLD (finger resting on the glass) */
        if (mpress && !mdown) { press_ms = now_ms; press_x = mx; press_y = my; }
        if (mpress && now_ms - press_ms > 300 && abs(my - press_y) < 30) tank_touch_hold(&tank, (float)mx, (float)my);
        if (!mpress && mdown) {
            int dx = mx - press_x, dy = my - press_y;
            if (milestones_view) { milestones_view = false; }
            else if (now_ms - press_ms < 250 && dx * dx + dy * dy < 64) {
                int best = -1; float bd = 26 * 26;
                for (int i = 0; i < tank.n_fish; i++) {
                    float fx = tank.fish[i].x - mx, fy = tank.fish[i].y - my;
                    if (fx * fx + fy * fy < bd) { bd = fx * fx + fy * fy; best = i; }
                }
                if (best >= 0) selected_fish = (best == selected_fish) ? -1 : best;
                else tank_touch_tap(&tank, (float)mx, (float)my);
            } else if (press_y < 60 && dy >= 40) tank_feed(&tank, (float)mx, 3);
        }
        mdown = mpress;
        if (k[SDL_SCANCODE_U] && !udown) { ui_visible = !ui_visible; }
        udown = k[SDL_SCANCODE_U];
        if (k[SDL_SCANCODE_M] && !mkdown) { milestones_view = !milestones_view; }
        mkdown = k[SDL_SCANCODE_M];
        if (k[SDL_SCANCODE_R] && !rdown) { progression_force_arrival(&tank); print_roster(&tank); }
        rdown = k[SDL_SCANCODE_R];
        if (k[SDL_SCANCODE_Q] || k[SDL_SCANCODE_ESCAPE]) { progression_save(&tank); break; }
        if (k[SDL_SCANCODE_F] && !fdown) tank_feed(&tank, (float)mx, 3);
        if (k[SDL_SCANCODE_S] && !sdown && !tank.shadow.active) tank_start_shadow(&tank);
        if (k[SDL_SCANCODE_N] && !ndown) tank_toggle_light(&tank);
        if (k[SDL_SCANCODE_A]) tank_light_auto(&tank);
        if (k[SDL_SCANCODE_L] && !ldown && llm_available) {
            llm_active = !llm_active;
            printf("brain: %s\n", llm_active ? "LLM (14M student)" : "rules");
        }
        fdown = k[SDL_SCANCODE_F]; sdown = k[SDL_SCANCODE_S]; ndown = k[SDL_SCANCODE_N];
        ldown = k[SDL_SCANCODE_L];
        SDL_Delay(wait < 5 ? 5 : (wait > 16 ? 16 : wait));
    }
    return 0;
}
