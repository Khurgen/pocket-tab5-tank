/* main.c — PC simulator entry: LVGL v9 + SDL window, 448x368 to match the
 * ESP32 AMOLED (landscape). This is the only platform-specific file; tank.c,
 * advisor.c and render.c compile unchanged for firmware.
 *
 *   ./fishsim              run the tank (keys: F feed at the mouse x,
 *                          N light, L brain, U overlays, M milestones view,
 *                          X reset prompt (device: hold BOOT + tap the glass),
 *                          S the first-run setup flow (welcome / names / colours),
 *                          R force an arrival (the birth flow opens: announce /
 *                          name / family; S drops it), A auto-light, Q quit;
 *                          click fish = stats; tap the water surface = feed;
 *                          drag down from the top = feed; hold >= 3 s = finger
 *                          on glass (trusting fish visit); swipe sideways
 *                          through a canopy = trim that bed; drag = wipe algae;
 *                          3 quick taps = startle; 2 taps = light)
 *   ./fishsim --fresh      ignore the save (new tank: random pair)
 *   ./fishsim --fast N     tended time runs N x faster (stages, drift)
 *   ./fishsim --greedy     greedy decoding instead of sampling
 *   ./fishsim --selftest   headless reflex-layer check, no window
 *   ./fishsim --selftest-llm [min]   headless LLM path (real-time if min > 0)
 *   ./fishsim --selftest-pop         headless population/arrival/save check
 *   ./fishsim --selftest-sleep       headless sleep metabolism + ravenous begging
 *   ./fishsim --selftest-tend        headless canopy/algae/hold-attract check
 *   ./fishsim --selftest-hunger      headless hunger economy (untended tank never ravenous)
 *   ./fishsim --bench                headless render-cost profile (veg, card)
 *   (key Z: jump through 7 h of device-style sleep; key G: grow the canopy +
 *    algae now to try the chores - press again to cycle)
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <ctype.h>
#include "tank.h"
#include "advisor.h"
#include "advisor_core.h"
#include "render.h"
#include "progression.h"
#include "setup.h"

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
    render_milestones(&tank, fb, TANK_W); render_brightness_row(fb, TANK_W, 100);
    render_confirm_reset(fb, TANK_W, 0.5f);
    setup_begin(&tank);
    for (int pg = 0; pg < SETUP_PG_N; pg++) { render_setup(&tank, fb, TANK_W, 1.0f); if (pg + 1 < SETUP_PG_N) setup_activate(&tank, SETUP_HIT_NEXT); }
    setup_cancel(&tank);
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
    if (!progression_setup_pending()) { printf("FAIL: a new tank should owe the first-run setup\n"); return 1; }
    progression_time_scale = 600;                 /* 10 minutes of tended time per second */
    int arrivals = 0, last_n = tank.n_fish;
    bool saw_court = false;                       /* the tell fires before the fry */
    for (int i = 0; i < 60 * 60 * 6; i++) {       /* 6 sim-minutes */
        tank_tick(&tank, 1.0f / 60.0f, advisor_rules);
        /* an attentive keeper: feeds often, rests a finger by a fish */
        if (i % 300 == 0) tank_feed(&tank, 150 + (i % 900) / 3, 2);
        if (i % 120 < 100) tank_touch_hold(&tank, tank.fish[0].x + 10, tank.fish[0].y);
        progression_tick(&tank, 1.0f / 60.0f);
        saw_court |= (tank.courting && arrivals == 0);
        if (tank.n_fish != last_n) {
            printf("  t=%.0fs arrival: %s (%s) bold %.2f social %.2f -> %d fish\n", tank.clock,
                   tank.fish[tank.n_fish - 1].name, STAGE_NAMES[tank.fish[tank.n_fish - 1].stage],
                   tank.fish[tank.n_fish - 1].bold, tank.fish[tank.n_fish - 1].sociable, tank.n_fish);
            last_n = tank.n_fish; arrivals++;
        }
    }
    printf("selftest-pop: %d arrivals, %d fish, feedings %d, hold-approaches %d, pending %d, courted %d\n",
           arrivals, tank.n_fish, tank.player_feedings, tank.hold_approaches, progression_arrival_pending(), saw_court);
    if (arrivals > 0 && !saw_court) { printf("FAIL: no courtship tell before the first arrival\n"); return 1; }
    for (int i = 0; i < tank.n_fish; i++)
        printf("  %-5s %s age %.0fh size %.2f ms 0x%03x trust %.1f\n", tank.fish[i].name,
               STAGE_NAMES[tank.fish[i].stage], progression_age_s(&tank, i) / 3600, tank.fish[i].size,
               tank.fish[i].ms_bits, tank.fish[i].trust);
    printf("  tank ms 0x%03x\n", tank.tank_ms_bits);
    if (arrivals < 1) { printf("FAIL: no arrival earned by an attentive keeper\n"); return 1; }
    /* the newest fry is owed its welcome (the birth flow) and wears the
       family's colours: the body of one parent, the markings of the other */
    int nb = progression_newborn();
    if (nb != tank.n_fish - 1) { printf("FAIL: the last arrival (%d) is not owed the birth flow (%d)\n", tank.n_fish - 1, nb); return 1; }
    {
        const fish_t *f = &tank.fish[nb];
        if (f->parent_a < 0 || f->parent_b < 0 || f->parent_a >= nb || f->parent_b >= nb) { printf("FAIL: newborn parents %d/%d\n", f->parent_a, f->parent_b); return 1; }
        const fish_t *pa = &tank.fish[f->parent_a], *pb = &tank.fish[f->parent_b];
        if (f->color != pa->color || (f->accent != pb->accent && pb->accent != pa->color)) {   /* (markings step aside from a matching body) */
            printf("FAIL: newborn look %06x/%06x is not %s's body + %s's markings (%06x/%06x)\n", f->color, f->accent, pa->name, pb->name, pa->color, pb->accent); return 1;
        }
        printf("  newborn %s: body from %s, markings from %s, bold %.2f (%.2f/%.2f) social %.2f (%.2f/%.2f)\n", f->name, pa->name, pb->name,
               f->bold, pa->bold, pb->bold, f->sociable, pa->sociable, pb->sociable);
    }
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
            fabsf(tank.fish[i].bold - saved.fish[i].bold) > 1e-4f ||
            tank.fish[i].parent_a != saved.fish[i].parent_a || tank.fish[i].parent_b != saved.fish[i].parent_b) {
            printf("FAIL: restore mismatch on fish %d\n", i); return 1;
        }
    /* the debt came back too (or moved to the fry delivered at boot) */
    if (progression_newborn() != tank.n_fish - 1) { printf("FAIL: the birth flow owed is %d after the reload, not %d\n", progression_newborn(), tank.n_fish - 1); return 1; }
    printf("  save/restore ok (%d fish, tank ms 0x%03x, fry %d owed its welcome)\n", tank.n_fish, tank.tank_ms_bits, progression_newborn());
    /* the birth flow (setup.c): the platforms poll it open; announce ->
       name (the wheel) -> family (BACK / DONE); DONE pays the debt and
       saves the name */
    {
        static uint16_t fb[TANK_W * TANK_H];
        nb = progression_newborn();
        if (setup_poll_birth(&tank) != nb || !setup_active() || !setup_is_birth() || setup_page() != SETUP_PG_BORN || setup_fish() != nb) {
            printf("FAIL: the birth flow did not open on the announcement (%d)\n", setup_page()); return 1;
        }
        if (setup_poll_birth(&tank) != -1) { printf("FAIL: the poll re-opened a flow already up\n"); return 1; }
        if (setup_hit(SETUP_MID_X + 20, SETUP_BTN_Y + 20) != SETUP_HIT_NEXT || setup_hit(SETUP_TOP_NEXT_X + 20, SETUP_TOP_BTN_Y + 20) != 0) { printf("FAIL: announcement hit-test\n"); return 1; }
        render_setup(&tank, fb, TANK_W, 1.0f);
        setup_activate(&tank, SETUP_HIT_NEXT);
        setup_touch(&tank, 0, 0, false);
        if (setup_page() != SETUP_PG_NAME_NEW || setup_fish() != nb || tank.stage_fish != nb) { printf("FAIL: MEET IT did not reach the fry's name page (page %d, stage %d)\n", setup_page(), tank.stage_fish); return 1; }
        char was[FISH_NAME_MAX + 1]; strcpy(was, tank.fish[nb].name);
        setup_activate(&tank, SETUP_HIT_SLOT0 + 0); setup_activate(&tank, SETUP_HIT_UP);   /* the first letter, one step on */
        if (!strcmp(tank.fish[nb].name, was) || setup_fish() != nb) { printf("FAIL: the wheel did not turn the fry's name\n"); return 1; }
        render_setup(&tank, fb, TANK_W, 1.0f);
        setup_activate(&tank, SETUP_HIT_NEXT);
        char named[FISH_NAME_MAX + 1]; strcpy(named, tank.fish[nb].name);
        if (setup_page() != SETUP_PG_FAMILY || tank.stage_fish != -1) { printf("FAIL: NEXT did not reach the family page\n"); return 1; }
        if (setup_hit(SETUP_NEXT_X + 20, SETUP_BTN_Y + 20) != SETUP_HIT_NEXT || setup_hit(SETUP_BACK_X + 20, SETUP_BTN_Y + 20) != SETUP_HIT_BACK ||
            setup_hit(SETUP_MID_X + 55, SETUP_FAM_ROW_Y) != 0) { printf("FAIL: family page hit-test\n"); return 1; }
        render_setup(&tank, fb, TANK_W, 1.0f);
        setup_activate(&tank, SETUP_HIT_BACK);
        if (setup_page() != SETUP_PG_NAME_NEW || strcmp(tank.fish[nb].name, named)) { printf("FAIL: BACK from the family page lost the name\n"); return 1; }
        setup_activate(&tank, SETUP_HIT_BACK);
        if (setup_page() != SETUP_PG_BORN) { printf("FAIL: BACK did not return to the announcement\n"); return 1; }
        setup_activate(&tank, SETUP_HIT_BACK);
        if (setup_page() != SETUP_PG_BORN) { printf("FAIL: BACK left the flow\n"); return 1; }
        setup_activate(&tank, SETUP_HIT_NEXT); setup_activate(&tank, SETUP_HIT_NEXT); setup_activate(&tank, SETUP_HIT_NEXT);   /* DONE */
        if (setup_active() || progression_newborn() != -1 || tank.stage_fish != -1) { printf("FAIL: DONE did not pay the birth debt\n"); return 1; }
        if (setup_poll_birth(&tank) != -1) { printf("FAIL: the poll opened a flow with nothing owed\n"); return 1; }
        tank_init(&tank, 6); progression_boot(&tank);
        if (progression_newborn() != -1 || strcmp(tank.fish[nb].name, named)) { printf("FAIL: the fry's name ('%s') or the paid debt (%d) did not survive a reboot\n", tank.fish[nb].name, progression_newborn()); return 1; }
        printf("  birth flow ok: %s (was %s), named through the wheel, saved and reloaded, nothing owed\n", named, was);
    }
    /* the keeper's reset: every save gone, two fry with nothing tended, and
       the fresh pair already saved so a reboot lands on them */
    progression_reset(&tank, 11);
    if (tank.n_fish != 2 || tank.fish[0].stage != STAGE_FRY || tank.fish[1].stage != STAGE_FRY ||
        tank.tank_ms_bits != TMS_PAIR || tank.player_feedings != 0 || progression_age_s(&tank, 0) != 0) {
        printf("FAIL: reset did not give a fresh pair\n"); return 1;
    }
    tank_t fresh = tank;
    tank_init(&tank, 12); progression_boot(&tank);
    if (tank.n_fish != 2 || tank.fish[0].preset != fresh.fish[0].preset ||
        tank.fish[1].preset != fresh.fish[1].preset || tank.tank_ms_bits != TMS_PAIR) {
        printf("FAIL: the reset tank did not come back from its save\n"); return 1;
    }
    if (render_confirm_hit(RENDER_CONFIRM_NO_X + 10, RENDER_CONFIRM_BTN_Y + 10) != -1 ||
        render_confirm_hit(RENDER_CONFIRM_YES_X + 10, RENDER_CONFIRM_BTN_Y + 10) != 1 ||
        render_confirm_hit(RENDER_CONFIRM_X + 5, RENDER_CONFIRM_Y + 5) != 0 || render_confirm_hit(5, 5) != 0) {
        printf("FAIL: confirm buttons hit-test\n"); return 1;
    }
    printf("  reset ok: %s + %s, both fry, saved and reloaded\n", tank.fish[0].name, tank.fish[1].name);
    /* the first-run setup (setup.c): the reset tank owes it; walk it the way
       a finger would - hit-test where a finger lands, activate what came
       back, drag the letter wheel through setup_touch - name the first fish
       BUB, give it the blue body and white accent, empty the second name
       (the preset's returns), BEGIN; then reload and find it all kept */
    if (!progression_setup_pending()) { printf("FAIL: the reset tank should owe the setup\n"); return 1; }
    setup_begin(&tank);
    if (!setup_active() || setup_page() != SETUP_PG_WELCOME) { printf("FAIL: setup did not open on the welcome page\n"); return 1; }
    if (setup_hit(SETUP_MID_X + 20, SETUP_BTN_Y + 20) != SETUP_HIT_NEXT || setup_hit(5, 5) != 0) { printf("FAIL: welcome NEXT hit-test\n"); return 1; }
    setup_activate(&tank, setup_hit(SETUP_MID_X + 20, SETUP_BTN_Y + 20));
    if (setup_page() != SETUP_PG_BUBBLES) { printf("FAIL: NEXT did not reach the bubbles page\n"); return 1; }
    /* the bubble column: a press on the water brings it there, a drag moves
       it, the rising bubbles come along, the reef and the grass push it off;
       the release is no tap */
    {
        setup_touch(&tank, 300, 200, true);
        tank_t probe = tank; tank_set_bubble_x(&probe, 300);
        if (fabsf(tank.bubble_x - probe.bubble_x) > 0.5f) { printf("FAIL: press put the column at %.0f, not %.0f\n", tank.bubble_x, probe.bubble_x); return 1; }
        for (int k = 1; k <= 20; k++) setup_touch(&tank, 300 + k * 2, 200, true);
        probe = tank; tank_set_bubble_x(&probe, 340);
        if (fabsf(tank.bubble_x - probe.bubble_x) > 0.5f) { printf("FAIL: drag left the column at %.0f\n", tank.bubble_x); return 1; }
        for (int i = 0; i < MAX_BUBBLE; i++)
            if (tank.bubble[i].column && fabsf(tank.bubble[i].x - tank.bubble_x) > 12) { printf("FAIL: a column bubble stayed behind\n"); return 1; }
        setup_touch(&tank, 340, 200, false);
        if (setup_page() != SETUP_PG_BUBBLES) { printf("FAIL: the drag's release acted as a tap\n"); return 1; }
        tank_set_bubble_x(&tank, 5);
        if (tank.bubble_x < tank.reef_x + 50) { printf("FAIL: the column sits on the reef (%.0f)\n", tank.bubble_x); return 1; }
        tank_set_bubble_x(&tank, 900);
        if (tank.bubble_x > TANK_W - 30) { printf("FAIL: the column sits in the glass (%.0f)\n", tank.bubble_x); return 1; }
        tank_set_bubble_x(&tank, 340);
    }
    float bubble_x_set = tank.bubble_x;
    setup_activate(&tank, SETUP_HIT_NEXT);
    if (setup_page() != SETUP_PG_NAME_A) { printf("FAIL: NEXT did not reach the name page\n"); return 1; }
    /* the fish being named takes the stage - the clear spot above the letters */
    setup_touch(&tank, 0, 0, false);
    if (tank.stage_fish != 0) { printf("FAIL: fish 0 not staged\n"); return 1; }
    tank.fish[0].x = 60; tank.fish[0].y = 300;
    for (int i = 0; i < 900; i++) tank_tick(&tank, 1.0f / 60.0f, advisor_rules);
    if (tank_dist(tank.fish[0].x, tank.fish[0].y, SETUP_STAGE_X, SETUP_STAGE_NAME_Y) > 45) {
        printf("FAIL: staged fish at (%.0f,%.0f), stage (%d,%d)\n", tank.fish[0].x, tank.fish[0].y, SETUP_STAGE_X, SETUP_STAGE_NAME_Y); return 1;
    }
    const char *preset0 = tank_roster_name(tank.fish[0].preset);
    /* the wheel's bands: the row picks a slot, above / below the active one are the chevrons */
    if (setup_hit(SETUP_SLOT_X + 2 * SETUP_SLOT_PX + 10, SETUP_SLOT_Y + 20) != SETUP_HIT_SLOT0 + 2 ||
        setup_hit(SETUP_SLOT_X + 15, SETUP_SLOT_Y - 50) != SETUP_HIT_UP ||
        setup_hit(SETUP_SLOT_X + 15, SETUP_SLOT_Y + SETUP_SLOT_H + 60) != SETUP_HIT_DOWN ||
        setup_hit(SETUP_TOP_NEXT_X + 20, SETUP_TOP_BTN_Y + 20) != SETUP_HIT_NEXT) { printf("FAIL: name page hit-test\n"); return 1; }
    for (int i = 0; i < 3; i++) {                     /* B U B by spinning three slots */
        setup_activate(&tank, SETUP_HIT_SLOT0 + i);
        if (setup_slot() != i) { printf("FAIL: slot %d not picked\n", i); return 1; }
        int guard = 30;
        while (guard-- && toupper((unsigned char)tank.fish[0].name[i]) != "BUB"[i]) setup_activate(&tank, SETUP_HIT_UP);
    }
    while ((int)strlen(tank.fish[0].name) > 3) {       /* a longer preset: blank the tail */
        setup_activate(&tank, SETUP_HIT_SLOT0 + 3);
        int guard = 30;
        while (guard-- && (int)strlen(tank.fish[0].name) > 3) setup_activate(&tank, SETUP_HIT_DOWN);
    }
    if (strcmp(tank.fish[0].name, "BUB")) { printf("FAIL: spun name is '%s'\n", tank.fish[0].name); return 1; }
    /* the drag: press on slot 2, pull up three steps (B -> E), no tap fires on
       release; pull back down three: B again */
    {
        float sx = SETUP_SLOT_X + 2 * SETUP_SLOT_PX + 15, sy = SETUP_SLOT_Y + 20;
        setup_touch(&tank, sx, sy, true);
        for (int k = 1; k <= 30; k++) setup_touch(&tank, sx, sy - k * 3 * SETUP_SPIN_PX / 30.0f - 0.5f, true);
        if (strcmp(tank.fish[0].name, "BUE")) { printf("FAIL: drag up gave '%s'\n", tank.fish[0].name); return 1; }
        setup_touch(&tank, sx, sy - 3 * SETUP_SPIN_PX, false);
        if (strcmp(tank.fish[0].name, "BUE") || setup_slot() != 2) { printf("FAIL: the drag's release acted as a tap\n"); return 1; }
        setup_touch(&tank, sx, sy, true);
        for (int k = 1; k <= 30; k++) setup_touch(&tank, sx, sy + k * 3 * SETUP_SPIN_PX / 30.0f + 0.5f, true);
        setup_touch(&tank, sx, sy, false);
        if (strcmp(tank.fish[0].name, "BUB")) { printf("FAIL: drag down gave '%s'\n", tank.fish[0].name); return 1; }
    }
    setup_touch(&tank, SETUP_TOP_NEXT_X + 20, SETUP_TOP_BTN_Y + 20, true);     /* a tap through setup_touch */
    setup_touch(&tank, SETUP_TOP_NEXT_X + 22, SETUP_TOP_BTN_Y + 24, false);
    if (setup_page() != SETUP_PG_LOOK_A) { printf("FAIL: NEXT did not reach the look page\n"); return 1; }
    /* the swatch row takes a finger 30 px under it (fingers land low), never the "?" */
    if (setup_hit(SETUP_SW_X + 6 * SETUP_SW_PX + 20, SETUP_SW_Y + SETUP_SW_H + 30) != SETUP_HIT_BODY0 + 6 ||
        setup_hit(SETUP_SW_X + 20, SETUP_ACC_Y + 30) != 0) { printf("FAIL: swatch row hit-test\n"); return 1; }
    setup_activate(&tank, setup_hit(SETUP_SW_X + 6 * SETUP_SW_PX + 20, SETUP_SW_Y + 25));
    uint32_t accent0 = tank.fish[0].accent;
    if (tank.fish[0].color != LOOK_BODY[6] || accent0 == LOOK_BODY[6]) {
        printf("FAIL: swatch gave body %06x accent %06x\n", tank.fish[0].color, tank.fish[0].accent); return 1;
    }
    /* a body the same as the accent: the accent moves (stripes must show) */
    tank_set_look(&tank, 0, accent0, 0);
    if (tank.fish[0].accent == accent0) { printf("FAIL: accent did not step aside for a matching body\n"); return 1; }
    tank_set_look(&tank, 0, LOOK_BODY[6], accent0);
    setup_activate(&tank, SETUP_HIT_BACK);            /* back to the name page and forward again: name kept */
    if (setup_page() != SETUP_PG_NAME_A || strcmp(tank.fish[0].name, "BUB")) { printf("FAIL: BACK lost the name\n"); return 1; }
    setup_activate(&tank, SETUP_HIT_NEXT); setup_activate(&tank, SETUP_HIT_NEXT);
    if (setup_page() != SETUP_PG_NAME_B) { printf("FAIL: not on the second name page\n"); return 1; }
    const char *preset1 = tank_roster_name(tank.fish[1].preset);
    for (int i = (int)strlen(tank.fish[1].name) - 1; i >= 0; i--) {   /* blank it from the end... */
        setup_activate(&tank, SETUP_HIT_SLOT0 + i);
        int guard = 30;
        while (guard-- && (int)strlen(tank.fish[1].name) > i) setup_activate(&tank, SETUP_HIT_DOWN);
    }
    if (tank.fish[1].name[0]) { printf("FAIL: could not blank the name ('%s')\n", tank.fish[1].name); return 1; }
    setup_activate(&tank, SETUP_HIT_NEXT);                                  /* ...and leave it empty: the preset's returns */
    if (strcmp(tank.fish[1].name, preset1)) { printf("FAIL: empty name did not fall back ('%s')\n", tank.fish[1].name); return 1; }
    setup_activate(&tank, SETUP_HIT_NEXT);
    if (setup_page() != SETUP_PG_CARE) { printf("FAIL: not on the care page (%d)\n", setup_page()); return 1; }
    setup_activate(&tank, SETUP_HIT_NEXT);            /* BEGIN */
    if (setup_active() || progression_setup_pending() || tank.stage_fish != -1) { printf("FAIL: BEGIN did not finish the setup\n"); return 1; }
    tank_init(&tank, 13); progression_boot(&tank);
    if (fabsf(tank.bubble_x - bubble_x_set) > 0.5f) { printf("FAIL: the bubble column did not come back from the save (%.0f)\n", tank.bubble_x); return 1; }
    if (progression_setup_pending() || strcmp(tank.fish[0].name, "BUB") || strcmp(tank.fish[1].name, preset1) ||
        tank.fish[0].color != LOOK_BODY[6] || tank.fish[0].accent != accent0 || strcmp(preset0, tank_roster_name(tank.fish[0].preset))) {
        printf("FAIL: names/looks did not come back from the save ('%s' %06x/%06x, pending %d)\n",
               tank.fish[0].name, tank.fish[0].color, tank.fish[0].accent, progression_setup_pending()); return 1;
    }
    printf("  setup ok: %s (blue) + %s, bubbles at x %.0f, saved and reloaded, nothing owed\n", tank.fish[0].name, tank.fish[1].name, tank.bubble_x);
    (void)system(cmd);                            /* leave no test save behind */
    return 0;
}

/* sleep metabolism + ravenous begging, headless (the device drowse path):
 * a long dark gap starves everyone -> they beg at the surface, the trickle
 * holds off, and the keeper's first pellets end the wait. */
static int selftest_sleep(void) {
    setenv("POCKET_TANK_SAVE", "/tmp/pocket-tank-selftest.sav", 1);
    char cmd[600]; snprintf(cmd, sizeof cmd, "rm -f /tmp/pocket-tank-selftest.sav"); (void)system(cmd);
    tank_init(&tank, 4242);
    progression_boot(&tank);
    for (int i = 0; i < 600; i++) { tank_tick(&tank, 1.0f / 60.0f, advisor_rules); progression_tick(&tank, 1.0f / 60.0f); }
    tank_tick_sleep(&tank, 12 * 3600);            /* a long night away */
    for (int i = 0; i < tank.n_fish; i++) {
        const fish_t *f = &tank.fish[i];
        if (f->hunger < 8.5f || f->energy < 9.9f) {
            printf("FAIL: after 12h sleep %s hunger %.1f energy %.1f\n", f->name, f->hunger, f->energy);
            return 1;
        }
    }
    for (int i = 0; i < MAX_FOOD; i++)
        if (tank.food[i].alive) { printf("FAIL: pellet survived the night\n"); return 1; }
    /* wake: begging engages, everyone rises to the surface, no self-serve */
    float avg_y0 = 0;
    for (int i = 0; i < tank.n_fish; i++) avg_y0 += tank.fish[i].y / tank.n_fish;
    for (int step = 1; step <= 600; step++) {      /* 10 s awake */
        tank_tick(&tank, 1.0f / 60.0f, advisor_rules);
        progression_tick(&tank, 1.0f / 60.0f);
        if (step == 120 && !tank.ravenous) { printf("FAIL: not ravenous after starving sleep\n"); return 1; }
    }
    float avg_y = 0; int food_n = 0;
    for (int i = 0; i < tank.n_fish; i++) avg_y += tank.fish[i].y / tank.n_fish;
    for (int i = 0; i < MAX_FOOD; i++) food_n += tank.food[i].alive;
    printf("selftest-sleep: begging avg y %.0f (was %.0f), trickle held (%d pellets)\n", avg_y, avg_y0, food_n);
    if (avg_y > 100) { printf("FAIL: fish not waiting at the surface\n"); return 1; }
    if (food_n) { printf("FAIL: trickle fed a begging tank\n"); return 1; }
    /* the keeper arrives: starving fish DASH for the fresh pellets (the
     * frenzy presentation), and feeding everyone ends the state */
    tank_feed(&tank, TANK_W * 0.5f, 4);
    int fed_at = -1; float dash_speed = 0;
    for (int step = 1; step <= 2400 && fed_at < 0; step++) {   /* 40 s: one fish may gobble
                                                                     every pellet; the trickle
                                                                     then serves the other */
        tank_tick(&tank, 1.0f / 60.0f, advisor_rules);
        progression_tick(&tank, 1.0f / 60.0f);
        if (step <= 30)                               /* within 0.5 s of the drop */
            for (int i = 0; i < tank.n_fish; i++)
                if (tank.fish[i].hunger > 6.5f && tank.fish[i].target_speed > dash_speed)
                    dash_speed = tank.fish[i].target_speed;
        if (!tank.ravenous) fed_at = step;
    }
    if (fed_at < 0) {
        printf("FAIL: feeding did not end the begging\n");
        for (int i = 0; i < tank.n_fish; i++)
            printf("  DEBUG %s hunger %.1f at (%.0f,%.0f) speed %.0f goal %s size %.2f\n", tank.fish[i].name, tank.fish[i].hunger,
                   tank.fish[i].x, tank.fish[i].y, tank.fish[i].speed, GOAL_NAMES[tank.fish[i].goal.id], tank.fish[i].size);
        for (int i = 0; i < MAX_FOOD; i++) if (tank.food[i].alive) printf("  DEBUG pellet (%.0f,%.0f) age %.0f\n", tank.food[i].x, tank.food[i].y, tank.food[i].age);
        printf("  DEBUG ravenous %d feed_spot %.0f\n", tank.ravenous, tank.feed_spot_x);
        return 1;
    }
    /* the device's deep-sleep wake (2026-09-14): save, forget everything,
       come back "8 hours later" - the night is lived through in one step,
       NOT the cold-boot ravenous rule (which would pin everyone at 9.6) */
    {
        for (int i = 0; i < tank.n_fish; i++) tank.fish[i].hunger = 2.0f;
        tank_veg_set(&tank, 1, 0.30f);                   /* the 12 h above grew it to the ceiling */
        progression_save(&tank);
        float veg0 = tank.veg_growth[1];
        tank_init(&tank, 8);
        float h = progression_wake(&tank, clock_port_now_unix() + 8 * 3600);
        if (h < 7.99f || h > 8.01f) { printf("FAIL: wake simulated %.2f h, not 8\n", h); return 1; }
        for (int i = 0; i < tank.n_fish; i++)
            if (fabsf(tank.fish[i].hunger - 8.4f) > 0.15f) {     /* 2.0 + 8 h x 0.8/h */
                printf("FAIL: after an 8 h wake %s hunger %.1f (want 8.4)\n", tank.fish[i].name, tank.fish[i].hunger); return 1; }
        if (tank.veg_growth[1] <= veg0) { printf("FAIL: the grass did not grow through the night\n"); return 1; }
        printf("selftest-sleep: deep-sleep wake lived through %.0f h (hunger 2.0 -> %.1f, bed 1 %.2f -> %.2f)\n",
               h, tank.fish[0].hunger, veg0, tank.veg_growth[1]);
    }
    printf("selftest-sleep: dash %.0f px/s at the drop; fed and calmed %.1f s after pellets\n",
           dash_speed, fed_at / 60.0f);
    if (dash_speed < 80) { printf("FAIL: no feeding-frenzy dash (%.0f px/s)\n", dash_speed); return 1; }
    (void)system(cmd);
    return 0;
}

/* upkeep chores + the settled-hold gate, headless: sleep grows the canopy
 * and algae; taps trim a bed to nubs (never bare); a drag wipes the glass;
 * the canopy comfort band moves stress both ways; a hold only draws fish in
 * after ~3 s, and never a starving one. */
static int selftest_tend(void) {
    setenv("POCKET_TANK_SAVE", "/tmp/pocket-tank-selftest.sav", 1);
    char cmd[600]; snprintf(cmd, sizeof cmd, "rm -f /tmp/pocket-tank-selftest.sav"); (void)system(cmd);
    tank_init(&tank, 777);
    progression_boot(&tank);
    int film0 = 0;
    for (int i = 0; i < ALGAE_CELLS; i++) film0 += tank.algae[i] > 0;
    if (film0) { printf("FAIL: fresh glass not clean (%d cells)\n", film0); return 1; }
    float g0 = tank.veg_growth[1];
    /* a night of drowse lets the garden get away */
    tank_tick_sleep(&tank, 7 * 3600);
    int film = 0;
    for (int i = 0; i < ALGAE_CELLS; i++) film += tank.algae[i] > 0;
    printf("selftest-tend: after 7 h sleep, canopy %.2f -> %.2f, %d algae cells\n",
           g0, tank.veg_growth[1], film);
    if (tank.veg_growth[1] <= g0 + 0.3f) { printf("FAIL: canopy barely grew in sleep\n"); return 1; }
    if (film < 10) { printf("FAIL: algae did not film the glass\n"); return 1; }
    /* the device drowses in 30 s slices (firmware DROWSE_TICK_US): the same
     * night delivered that way must film the glass just as much. Before
     * 2026-09-04 every slice truncated to 0 film steps and the glass stayed
     * clean forever on the hardware. */
    {
        tank_t sliced; tank_init(&sliced, 777); progression_boot(&sliced);
        for (int i = 0; i < 7 * 120; i++) tank_tick_sleep(&sliced, 30.0f);
        int f2 = 0;
        for (int i = 0; i < ALGAE_CELLS; i++) f2 += sliced.algae[i] > 0;
        printf("selftest-tend: the same night in 30 s drowse slices: %d algae cells\n", f2);
        if (f2 < film / 2) { printf("FAIL: drowse slices do not grow algae\n"); return 1; }
    }
    /* height is growth: a full bed's tallest frond touches the ceiling */
    {
        float ty; int ms;
        for (int b = 0; b < VEG_BEDS; b++) tank_veg_set(&tank, b, 1.0f);
        tank_veg_bed(&tank, 2, NULL, NULL, &ty, NULL);
        ms = tank_veg_frond(&tank, 2, 0, NULL);
        printf("selftest-tend: full bed 2: %d segments, canopy top y %.0f\n", ms, ty);
        if (ty > 24) { printf("FAIL: a full bed should reach the ceiling (top %.0f)\n", ty); return 1; }
        tank_tick_sleep(&tank, 7 * 3600);          /* restore the grown state for the trims */
    }
    /* a TAP in the canopy must NOT trim (that was the accidental-cut bug:
     * missed pokes at a fish were shearing the garden) */
    {
        float x0, x1, ty;
        tank_veg_bed(&tank, 1, &x0, &x1, &ty, NULL);
        float g_before = tank.veg_growth[1];
        tank_touch_tap(&tank, (x0 + x1) * 0.5f, (ty + TANK_H) * 0.5f);
        if (tank.veg_growth[1] != g_before) { printf("FAIL: a tap trimmed the canopy\n"); return 1; }
        tank.tap_count = 0; tank.tap_burst_t = 99;   /* don't leak into later gestures */
    }
    /* an algae scrub that starts mid-glass and sweeps across the whole floor
     * must NOT cut anything (the accidental mow-the-garden bug): a slash only
     * arms the bed the stroke STARTED on */
    {
        float g0[VEG_BEDS]; memcpy(g0, tank.veg_growth, sizeof g0);
        for (float sx = 2; sx < TANK_W - 2; sx += 6)     /* one full-width stroke */
            tank_touch_drag(&tank, sx, TANK_H - 30.0f);
        tank_tick(&tank, 1.0f / 60.0f, advisor_rules);
        tank_tick(&tank, 1.0f / 60.0f, advisor_rules);
        for (int b = 0; b < VEG_BEDS; b++)
            if (tank.veg_growth[b] < g0[b]) {
                printf("FAIL: a mid-glass scrub cut bed %d\n", b); return 1;
            }
    }
    /* trimming (2026-09-04, per frond): a sideways stroke that starts on a
     * bed cuts exactly the fronds it crosses, at the height it crosses them.
     * A cleaning scrub that starts over a bed but not beside a frond (bed 1:
     * frond 0 at the ceiling, the rest short; the finger lands mid-glass in
     * the short fronds' column, above their tips) and zigzags down to the
     * floor must cut NOTHING (2026-09-04: the old bed-box arming sheared the
     * garden whenever one frond was tall) */
    {
        tank_veg_set(&tank, 1, 0.3f); tank.veg_h[1][0] = 1.0f; tank_veg_sync(&tank);
        float before[VEG_FRONDS_MAX]; memcpy(before, tank.veg_h[1], sizeof before);
        float fx2; tank_veg_frond(&tank, 1, 2, &fx2);
        for (float y = 150; y <= TANK_H - 10; y += 6)
            tank_touch_drag(&tank, fx2 + ((int)(y / 6) & 1 ? 14 : -14), y);
        tank_tick(&tank, 1.0f / 60.0f, advisor_rules); tank_tick(&tank, 1.0f / 60.0f, advisor_rules);
        int n; tank_veg_bed(&tank, 1, NULL, NULL, NULL, &n);
        for (int i = 0; i < n; i++)
            if (tank.veg_h[1][i] < before[i] - 0.001f) { printf("FAIL: scrub from mid-glass cut frond %d (%.2f -> %.2f)\n", i, before[i], tank.veg_h[1][i]); return 1; }
        printf("selftest-tend: a scrub begun mid-glass over the bed cut nothing\n");
        tank_veg_set(&tank, 1, 1.0f);
    }
    /* First the precision: a short flick across fronds 1 and 2 of bed 1 at
     * mid-height takes those two to that height and touches nothing else */
    {
        float fx1, fx2; tank_veg_frond(&tank, 1, 1, &fx1); tank_veg_frond(&tank, 1, 2, &fx2);
        float before[VEG_FRONDS_MAX]; memcpy(before, tank.veg_h[1], sizeof before);
        float sy = 200;                            /* lands and lifts a third of a pitch off the two spines:
                                                      the pad's reach (SLASH_REACH_PX) must not take 0 or 3 */
        for (float sx = fx1 - 3; sx <= fx2 + 3; sx += 2) tank_touch_drag(&tank, sx, sy);
        tank_tick(&tank, 1.0f / 60.0f, advisor_rules); tank_tick(&tank, 1.0f / 60.0f, advisor_rules);
        float want = (TANK_H - 16 - sy) / ((VEG_SEGS_FULL - 1) * 3.2f);
        int n; tank_veg_bed(&tank, 1, NULL, NULL, NULL, &n);
        printf("selftest-tend: flick at y %.0f: bed 1 fronds 1,2 %.2f/%.2f -> %.2f/%.2f (want %.2f); frond 0 %.2f -> %.2f\n",
               sy, before[1], before[2], tank.veg_h[1][1], tank.veg_h[1][2], want, before[0], tank.veg_h[1][0]);
        for (int i = 0; i < n; i++) {
            bool cut = i == 1 || i == 2;
            float h = tank.veg_h[1][i];
            if (cut && fabsf(h - want) > 0.02f) { printf("FAIL: frond %d not cut to the finger's height (%.2f vs %.2f)\n", i, h, want); return 1; }
            if (!cut && h < before[i]) { printf("FAIL: frond %d cut by a flick that never crossed it\n", i); return 1; }
        }
    }
    /* The outer blade by the glass (Strato, 2026-09-14: "the last blade in the
     * row never trips"): bed 1's last frond stands 24 px from the glass and
     * the finger's reported centre stops short of its spine. A sweep along
     * the floor that ends 6 px before that spine must still take it... */
    {
        int n; float x0; tank_veg_bed(&tank, 1, &x0, NULL, NULL, &n);
        float fl; tank_veg_frond(&tank, 1, n - 1, &fl);
        tank_veg_set(&tank, 1, 1.0f);
        for (float sx = x0 + 2; sx <= fl - 6; sx += 4) tank_touch_drag(&tank, sx, TANK_H - 8.0f);
        tank_tick(&tank, 1.0f / 60.0f, advisor_rules); tank_tick(&tank, 1.0f / 60.0f, advisor_rules);
        printf("selftest-tend: sweep stopping 6 px short of bed 1's outer frond (spine %.0f, glass %d): %.2f\n", fl, TANK_W, tank.veg_h[1][n - 1]);
        if (tank.veg_h[1][n - 1] > VEG_NUB + 1e-3f) { printf("FAIL: the outer frond stood after a sweep that stopped just short of it\n"); return 1; }
        /* ...and a stroke that BEGINS between that frond and the glass, 14 px
         * out, heading in, arms and takes it (it used to arm nothing) */
        tank_veg_set(&tank, 1, 1.0f);
        for (float sx = fl + 14; sx >= x0; sx -= 4) tank_touch_drag(&tank, sx, TANK_H - 8.0f);
        tank_tick(&tank, 1.0f / 60.0f, advisor_rules); tank_tick(&tank, 1.0f / 60.0f, advisor_rules);
        if (fabsf(tank.veg_growth[1] - VEG_NUB) > 1e-3f) { printf("FAIL: a sweep begun at the glass left bed 1 at %.2f\n", tank.veg_growth[1]); return 1; }
        printf("selftest-tend: a sweep begun 14 px outside the outer frond mowed the bed\n");
        tank_veg_set(&tank, 1, 1.0f);
    }
    /* ...then the mow: one sweep along the floor takes every frond of a bed
     * to nubs, never bare */
    for (int b = 0; b < VEG_BEDS; b++) {
        float x0, x1, ty;
        tank_veg_bed(&tank, b, &x0, &x1, &ty, NULL);
        for (float sx = x0 + 2; sx <= x1; sx += 4) tank_touch_drag(&tank, sx, TANK_H - 8.0f);
        tank_tick(&tank, 1.0f / 60.0f, advisor_rules);   /* consume the stroke... */
        tank_tick(&tank, 1.0f / 60.0f, advisor_rules);   /* ...and reset for the next */
        if (fabsf(tank.veg_growth[b] - VEG_NUB) > 1e-3f) {   /* it keeps growing a hair per tick */
            printf("FAIL: bed %d not mowed to nubs (%.2f)\n", b, tank.veg_growth[b]); return 1;
        }
    }
    if (tank.startled) { printf("FAIL: trimming spooked the tank\n"); return 1; }
    /* the canopy comfort band, all three regimes */
    fish_t *cf = &tank.fish[0];
    for (int b = 0; b < VEG_BEDS; b++) tank_veg_set(&tank, b, 1.0f);      /* jungle */
    cf->stress = 0;
    for (int i = 0; i < 60 * 30; i++) {
        tank_tick(&tank, 1.0f / 60.0f, advisor_rules);
    }
    float s_jungle = cf->stress;
    for (int b = 0; b < VEG_BEDS; b++) tank_veg_set(&tank, b, VEG_NUB);   /* scalped bare */
    cf->stress = 0;
    for (int i = 0; i < 60 * 60; i++) {
        tank_tick(&tank, 1.0f / 60.0f, advisor_rules);
    }
    float s_bare = cf->stress;
    for (int b = 0; b < VEG_BEDS; b++) tank_veg_set(&tank, b, 0.40f);     /* comfortable */
    cf->stress = 5;
    for (int i = 0; i < 60 * 30; i++) {
        tank_tick(&tank, 1.0f / 60.0f, advisor_rules);
    }
    float s_comfy = cf->stress;
    /* the smother threshold (2026-09-04): TWO beds at 90% is smothered ... */
    tank_veg_set(&tank, 0, 0.9f); tank_veg_set(&tank, 1, 0.9f); tank_veg_set(&tank, 2, 0.3f);
    cf->stress = 0;
    for (int i = 0; i < 60 * 60; i++) {
        tank_tick(&tank, 1.0f / 60.0f, advisor_rules);
    }
    float s_two = cf->stress;
    /* ... but ONE bed at the ceiling with the others tall (under 85%) is
     * just a lot of good cover: stress must fall, faster than in open water */
    tank_veg_set(&tank, 0, 1.0f); tank_veg_set(&tank, 1, 0.8f); tank_veg_set(&tank, 2, 0.8f);
    cf->stress = 5; cf->x = TANK_W * 0.5f; cf->y = 60;   /* open water, not hidden */
    for (int i = 0; i < 60 * 8; i++) {
        tank_tick(&tank, 1.0f / 60.0f, advisor_rules);
        cf->x = TANK_W * 0.5f; cf->y = 60; cf->goal.id = GOAL_EXPLORE;   /* not resting: base decay only */
    }
    float s_one = cf->stress;
    for (int b = 0; b < VEG_BEDS; b++) tank_veg_set(&tank, b, VEG_BARE);   /* bare-ish: base decay only */
    cf->stress = 5;
    for (int i = 0; i < 60 * 8; i++) {
        tank_tick(&tank, 1.0f / 60.0f, advisor_rules);
        cf->x = TANK_W * 0.5f; cf->y = 60; cf->goal.id = GOAL_EXPLORE;
    }
    float s_open = cf->stress;
    printf("selftest-tend: stress jungle %.1f, bare %.1f (mild), comfy %.1f | two beds 90%% %.1f, one full bed %.1f (open water 8 s from 5: cover %.1f vs none %.1f)\n",
           s_jungle, s_bare, s_comfy, s_two, s_one, s_one, s_open);
    if (s_jungle < 4.0f) { printf("FAIL: overgrown tank not stressful\n"); return 1; }
    if (s_bare < 0.5f || s_bare > 3.5f) { printf("FAIL: bare tank should be mildly uneasy\n"); return 1; }
    if (s_comfy > 1.5f) { printf("FAIL: comfortable canopy should let stress decay\n"); return 1; }
    if (s_two < 1.5f) { printf("FAIL: two beds past 85%% should smother\n"); return 1; }
    if (s_one >= s_open) { printf("FAIL: one tall bed should calm, not stress\n"); return 1; }
    /* cleaning: squeegee strokes across every row of the glass */
    for (int y = 8; y < TANK_H; y += ALGAE_CELL) {
        for (int x = 0; x <= TANK_W; x += 8) tank_touch_drag(&tank, (float)x, (float)y);
        tank_tick(&tank, 1.0f / 60.0f, advisor_rules);   /* consume: stroke ends */
        progression_tick(&tank, 1.0f / 60.0f);
    }
    film = 0;
    for (int i = 0; i < ALGAE_CELLS; i++) film += tank.algae[i] > 0;
    if (film) { printf("FAIL: %d algae cells survived the wipe\n", film); return 1; }
    if (!(tank.tank_ms_bits & TMS_FIRST_TRIM) || !(tank.tank_ms_bits & TMS_FIRST_CLEANING)) {
        printf("FAIL: upkeep milestones not detected (ms 0x%03x)\n", tank.tank_ms_bits); return 1;
    }
    printf("selftest-tend: trimmed + wiped clean (trims %d, cells %d, tank ms 0x%03x)\n",
           tank.trims, tank.cells_cleaned, tank.tank_ms_bits);
    /* upkeep survives a save round-trip */
    tank_veg_set(&tank, 0, 0.62f); tank_veg_set(&tank, 1, VEG_NUB); tank_veg_set(&tank, 2, 0.9f);
    tank_grow_algae(&tank, 40);
    progression_save(&tank);
    tank_t before = tank;
    tank_init(&tank, 9); progression_boot(&tank);
    if (memcmp(tank.algae, before.algae, ALGAE_CELLS) != 0 ||
        memcmp(tank.veg_h, before.veg_h, sizeof tank.veg_h) != 0 ||
        memcmp(tank.veg_growth, before.veg_growth, sizeof tank.veg_growth) != 0 ||
        tank.trims != before.trims || tank.cells_cleaned != before.cells_cleaned) {
        printf("FAIL: upkeep state lost in save round-trip\n"); return 1;
    }
    printf("selftest-tend: save round-trip ok\n");
    for (int b = 0; b < VEG_BEDS; b++) tank_veg_set(&tank, b, 0.40f);  /* comfy for the hold legs */
    /* the settled hold: fish[0] made calm + trusting, finger 90 px away.
     * Holds are reported ~0.3 s after contact (platform), so hold_time here
     * maps 1:1 to reported time; the gate is 2.7 s of hold_time. */
    fish_t *f = &tank.fish[0];
    f->trust = 9; f->hunger = 1; f->energy = 10; f->stress = 0;
    f->goal.id = GOAL_EXPLORE; f->goal.urgency = 2;
    tank.ravenous = false;              /* isolate the hold reflex (no progression_tick here) */
    f->x = 200; f->y = 200; float hx = 290, hy = 200;
    float d_early = -1, d_late = -1;
    for (int step = 1; step <= 60 * 8; step++) {
        tank_touch_hold(&tank, hx, hy);
        tank_tick(&tank, 1.0f / 60.0f, advisor_rules);
        f->hunger = 1;                                  /* keep the veto out of this leg */
        if (step == 60 * 2) d_early = tank_dist(f->x, f->y, hx, hy);
        if (step == 60 * 8) d_late = tank_dist(f->x, f->y, hx, hy);
    }
    printf("selftest-tend: hold dist at 2 s %.0f px, at 8 s %.0f px\n", d_early, d_late);
    if (d_early < 45) { printf("FAIL: fish approached before the 3 s gate\n"); return 1; }
    if (d_late > 45) { printf("FAIL: settled hold never drew the fish in\n"); return 1; }
    /* a starving fish ignores the finger */
    f->x = 200; f->y = 200; f->hunger = 9.4f;
    for (int i = 0; i < MAX_FOOD; i++) tank.food[i].alive = false;   /* nothing to chase */
    for (int step = 1; step <= 60 * 8; step++) {
        tank_touch_hold(&tank, hx, hy);
        tank_tick(&tank, 1.0f / 60.0f, advisor_rules);
        f->hunger = 9.4f;
        for (int i = 0; i < MAX_FOOD; i++) tank.food[i].alive = false;
    }
    float d_hungry = tank_dist(f->x, f->y, hx, hy);
    printf("selftest-tend: starving fish dist after 8 s hold %.0f px\n", d_hungry);
    if (d_hungry < 40) { printf("FAIL: a starving fish came to the finger\n"); return 1; }
    (void)system(cmd);
    return 0;
}

/* ---------- LVGL + SDL ---------- */
#include "lvgl/lvgl.h"
#include <SDL2/SDL.h>

static lv_draw_buf_t draw_buf;
static uint16_t canvas_buf[TANK_W * TANK_H];
static uint16_t scene_buf[TANK_W * TANK_H];
static uint8_t  vig_buf[TANK_W * TANK_H];     /* vignette LUT, as on the device */
static uint16_t card_buf[RENDER_CARD_W * RENDER_CARD_H];   /* stats card cache, as on the device */
static uint32_t dirty_buf[RENDER_DIRTY_WORDS];
static lv_obj_t *canvas;
static uint32_t last_ms;
static bool llm_available = false;
static bool llm_active = false;
static int  selected_fish = -1;      /* click a fish for its stat card */
static bool ui_visible = true;       /* U toggles all overlays */
static bool milestones_view = false; /* M toggles the milestones screen */
static bool confirm_view = false;    /* X: the reset prompt (YES wipes the save) */
static int  sim_bright = 100;        /* the milestones page's brightness row (device setting; cosmetic here) */
static uint32_t confirm_ms;          /* when it opened; it gives up after CONFIRM_MS */
#define CONFIRM_MS 20000

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
    tank.hold_light = setup_active() || confirm_view;   /* no lights-out mid-name */
    tank_tick(&tank, dt, llm_active ? advisor_llm : advisor_rules);
    progression_tick(&tank, dt);
    if (!confirm_view) {                          /* an arrival owed its welcome: the birth flow (setup.c) */
        int nb = setup_poll_birth(&tank);
        if (nb >= 0) { selected_fish = -1; milestones_view = false;
                       printf("a new fry, %s: the birth flow is up (announce / name / family; S drops it)\n", tank.fish[nb].name); }
    }
    if (milestones_view) { render_milestones(&tank, canvas_buf, TANK_W); render_brightness_row(canvas_buf, TANK_W, sim_bright); }
    else {
        render_tank(&tank, canvas_buf, TANK_W);
        if (ui_visible) {
            draw_brain_dot();
            if (selected_fish >= 0)
                render_stats_card(&tank, selected_fish, canvas_buf, TANK_W);
        }
    }
    if (setup_active()) render_setup(&tank, canvas_buf, TANK_W, tank.clock);
    if (confirm_view) render_confirm_reset(canvas_buf, TANK_W, 1.0f - (SDL_GetTicks() - confirm_ms) / (float)CONFIRM_MS);
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
    if (getenv("POCKET_CURIOUS")) {               /* reproduce a state: POCKET_CURIOUS=9 = the
                                                     device after days of the old economy */
        for (int i = 0; i < tank.n_fish; i++) { tank.fish[i].curiosity = (float)atof(getenv("POCKET_CURIOUS")); tank.fish[i].hunger = 3; }
        printf("start state: curiosity %s, hunger 3\n", getenv("POCKET_CURIOUS"));
    }
    print_roster(&tank);
    printf("warm-up (pages in the mmap'd weights):\n");
    advisor_llm_debug(&tank, 0);
    advisor_llm_debug(&tank, 1);
    int changes = 0, torn = 0;
    int ticks = minutes > 0 ? minutes * 3600 : 3600;
    int delay = minutes > 0 ? 16 : 2;             /* 16ms = real-time 60fps */
    goal_id_t last[N_FISH_MAX];
    for (int i = 0; i < tank.n_fish; i++) last[i] = tank.fish[i].goal.id;
    /* census (2026-09-01, Strato: "all four fish overlapping at the bubble
     * column almost all the time"): goal shares, time near the column, how
     * often 3+ fish crowd it, and the drives the model is reading */
    long goal_ticks[GOAL_COUNT] = {0}, near_ticks = 0, crowd_ticks = 0, cluster_ticks = 0; double cur_sum = 0, hun_sum = 0, en_sum = 0;
    for (int i = 0; i < ticks; i++) {
        tank_tick(&tank, 1.0f / 60.0f, advisor_llm);
        progression_tick(&tank, 1.0f / 60.0f);
        SDL_Delay(delay);
        int near_b = 0, near_r = 0, cluster = 0;
        for (int fi = 0; fi < tank.n_fish; fi++) {
            const fish_t *f = &tank.fish[fi];
            goal_ticks[f->goal.id]++;
            cur_sum += f->curiosity; hun_sum += f->hunger; en_sum += f->energy;
            if (tank_dist(f->x, f->y, tank.bubble_x, tank.bubble_y - 74) < 55) near_b++;
            if (tank_dist(f->x, f->y, tank.reef_x, tank.reef_y - 35) < 55) near_r++;
            int others = 0;                        /* the visual complaint: bodies overlapping */
            for (int fj = 0; fj < tank.n_fish; fj++)
                if (fj != fi && tank_dist(f->x, f->y, tank.fish[fj].x, tank.fish[fj].y) < 45) others++;
            if (others >= 2) cluster = 1;
        }
        near_ticks += near_b + near_r; if (near_b >= 3 || near_r >= 3) crowd_ticks++; cluster_ticks += cluster;
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
    printf("census: goal share");
    for (int g = 0; g < GOAL_COUNT; g++) printf(" %s %.0f%%", GOAL_NAMES[g], 100.0 * goal_ticks[g] / (ticks * tank.n_fish));
    printf("\ncensus: fish-time at a landmark (bubbles/reef, 55 px) %.0f%% | 3+ fish crowding one %.0f%% of the time | "
           "a 3-fish cluster anywhere %.0f%% | mean curiosity %.1f hunger %.1f energy %.1f\n",
           100.0 * near_ticks / (ticks * tank.n_fish), 100.0 * crowd_ticks / ticks, 100.0 * cluster_ticks / ticks,
           cur_sum / (ticks * tank.n_fish), hun_sum / (ticks * tank.n_fish), en_sum / (ticks * tank.n_fish));
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
    }
    /* show the upkeep systems: one overgrown bed, one mid, one trimmed to
     * nubs, and a patchy algae film; park a fish inside the reef canopy so
     * the front/back frond weave is visible */
    tank_veg_set(&tank, 0, 0.9f); tank_veg_set(&tank, 1, 0.5f); tank_veg_set(&tank, 2, VEG_NUB);
    tank.veg_h[0][4] = tank.veg_h[0][5] = 0.35f;   /* two fronds flick-trimmed at mid height */
    tank_grow_algae(&tank, 90);
    tank.fish[1].x = tank.reef_x + 16; tank.fish[1].y = TANK_H - 60;
    static uint16_t fb[TANK_W * TANK_H], scene[TANK_W * TANK_H];
    char path[512];
    render_set_scene_cache(scene);
    render_set_vignette_cache(vig_buf);
    render_set_card_cache(card_buf);
    render_set_dirty_mask(dirty_buf);
    render_tank(&tank, fb, TANK_W);
    snprintf(path, sizeof path, "%s_tank.ppm", prefix); write_ppm(path, fb);
    render_tank(&tank, fb, TANK_W); render_stats_card(&tank, 0, fb, TANK_W);
    snprintf(path, sizeof path, "%s_card.ppm", prefix); write_ppm(path, fb);
    render_tank(&tank, fb, TANK_W); render_stats_card(&tank, 1, fb, TANK_W);
    snprintf(path, sizeof path, "%s_card1.ppm", prefix); write_ppm(path, fb);   /* partly unrevealed */
    /* the milestones page: everything seen but one fish badge and one tank
       badge (they wear the "new" ring), and a tapped badge's caption */
    for (int i = 0; i < tank.n_fish; i++) tank.fish[i].ms_seen = tank.fish[i].ms_bits;
    tank.tank_ms_seen = tank.tank_ms_bits;
    tank.fish[1].ms_seen &= ~MS_FIRST_MEAL_FROM_YOU; tank.tank_ms_seen &= ~TMS_FIRST_QUIET_NIGHT;
    render_milestones(&tank, fb, TANK_W); render_brightness_row(fb, TANK_W, 60);
    snprintf(path, sizeof path, "%s_milestones.ppm", prefix); write_ppm(path, fb);
    render_milestones_tap(&tank, 176 + 16, 4 + 40 + 20);           /* fish 1's first badge -> the detail modal */
    render_milestones(&tank, fb, TANK_W); render_brightness_row(fb, TANK_W, 60);
    snprintf(path, sizeof path, "%s_milestone_modal.ppm", prefix); write_ppm(path, fb);
    render_milestones_leave();
    render_tank(&tank, fb, TANK_W); render_confirm_reset(fb, TANK_W, 0.7f);
    snprintf(path, sizeof path, "%s_confirm.ppm", prefix); write_ppm(path, fb);
    /* the first-run setup, page by page (never BEGIN: that would save this
       staged tank over the real one) */
    setup_begin(&tank);
    static const char *const pg_name[SETUP_PG_N] = { "welcome", "bubbles", "name", "look", "name2", "look2", "care" };
    for (int pg = 0; pg < SETUP_PG_N; pg++) {
        if (pg == SETUP_PG_BUBBLES) { setup_touch(&tank, 300, 200, true); setup_touch(&tank, 300, 200, false); }
        if (pg == SETUP_PG_NAME_A) { tank_set_name(&tank, 0, "BUB"); setup_activate(&tank, SETUP_HIT_SLOT0 + 1); }
        if (pg == SETUP_PG_LOOK_A) setup_activate(&tank, SETUP_HIT_BODY0 + 6);
        setup_touch(&tank, 0, 0, false);                   /* the stage is set; let the fish get there */
        for (int i = 0; i < 300; i++) tank_tick(&tank, 1.0f / 60.0f, advisor_rules);
        render_tank(&tank, fb, TANK_W); render_setup(&tank, fb, TANK_W, 1.0f);
        snprintf(path, sizeof path, "%s_setup_%s.ppm", prefix, pg_name[pg]); write_ppm(path, fb);
        if (pg + 1 < SETUP_PG_N) setup_activate(&tank, SETUP_HIT_NEXT);
    }
    setup_cancel(&tank);
    /* the birth flow, page by page (never DONE: it saves): the last fish as
       a fry just hatched in the reef bed's grass, born to fish 0 and 1
       (tank_add_fish above gave it their colours) */
    {
        int nb = tank.n_fish - 1;
        tank.fish[nb].stage = STAGE_FRY; tank.fish[nb].size = tank.fish[nb].base_size * 0.55f;
        tank.fish[nb].x = tank.reef_x + 70; tank.fish[nb].y = TANK_H - 34;
        setup_begin_birth(&tank, nb);
        static const char *const bpg_name[SETUP_BIRTH_PAGES] = { "born", "name_new", "family" };
        for (int pg = 0; pg < SETUP_BIRTH_PAGES; pg++) {
            setup_touch(&tank, 0, 0, false);
            if (setup_page() == SETUP_PG_NAME_NEW) for (int i = 0; i < 300; i++) tank_tick(&tank, 1.0f / 60.0f, advisor_rules);   /* let the fry reach the stage */
            render_tank(&tank, fb, TANK_W); render_setup(&tank, fb, TANK_W, 1.0f);
            snprintf(path, sizeof path, "%s_setup_%s.ppm", prefix, bpg_name[pg]); write_ppm(path, fb);
            if (pg + 1 < SETUP_BIRTH_PAGES) setup_activate(&tank, SETUP_HIT_NEXT);
        }
        setup_cancel(&tank);
    }
    printf("snapshot: %d fish, wrote %s_{tank,card,card1,milestones,confirm,setup_*}.ppm\n", tank.n_fish, prefix);
    return 0;
}


/* --selftest-hunger: the hunger economy (tank.c, 2026-09-01). An untended
 * 4-fish tank for 40 minutes of awake time under the rules brain: the
 * hunger-gated trickle must keep the school between "just fed" and
 * "peckish" - never ravenous (that is the after-sleep event), never the
 * old surface-hovering famine - and a keeper's feeding must make a fish
 * properly full. */
static int selftest_hunger(void) {
    tank_init(&tank, 99);
    tank_new_population(&tank);
    while (tank.n_fish < 4) tank_add_fish(&tank, 0, 1);
    for (int i = 0; i < tank.n_fish; i++) tank.fish[i].stage = STAGE_ADULT;
    const float dt = 1.0f / 25.0f;                 /* the device's frame rate */
    int ticks = (int)(40 * 60 / dt), rav_ticks = 0, peckish_ticks = 0, top_ticks = 0;
    float hmax = 0, hsum = 0; int pellets = 0, live_prev = 0;
    for (int i = 0; i < ticks; i++) {
        tank_tick(&tank, dt, advisor_rules);
        progression_tick(&tank, dt);
        int live = 0; for (int k = 0; k < MAX_FOOD; k++) live += tank.food[k].alive;
        if (live > live_prev) pellets += live - live_prev;
        live_prev = live;
        if (tank.ravenous) rav_ticks++;
        for (int k = 0; k < tank.n_fish; k++) {
            float h = tank.fish[k].hunger;
            if (h > hmax) hmax = h;
            hsum += h;
            if (h >= 7) peckish_ticks++;
            if (tank.fish[k].y < 45) top_ticks++;
        }
    }
    float mean = hsum / (ticks * tank.n_fish);
    float peck = 100.0f * peckish_ticks / (ticks * tank.n_fish);
    float top  = 100.0f * top_ticks / (ticks * tank.n_fish);
    printf("hunger: 40 min untended, 4 fish @25 fps: mean %.1f max %.1f | hungry(>=7) %.0f%% of fish-time | "
           "under the surface %.0f%% | trickle pellets %d | ravenous ticks %d\n",
           mean, hmax, peck, top, pellets, rav_ticks);
    if (rav_ticks > 0)  { printf("FAIL: an untended awake tank went ravenous\n"); return 1; }
    if (hmax > 8.6f)    { printf("FAIL: hunger reached %.1f (the trickle didn't keep up)\n", hmax); return 1; }
    if (mean > 7.0f)    { printf("FAIL: mean hunger %.1f - the school lives hungry\n", mean); return 1; }
    if (mean < 2.5f)    { printf("FAIL: mean hunger %.1f - the trickle feeds them for you\n", mean); return 1; }
    if (peck > 40.0f)   { printf("FAIL: fish are hungry %.0f%% of the time\n", peck); return 1; }
    /* the keeper feeds a HUNGRY fish: pellets land, it goes and eats (4.3
       hunger per pellet) and is comfortably fed a minute later. Judged on
       that fish, not the school's mean: the well-fed rest only ever met a
       pellet by wandering into one, which depended on how often the rule
       stub re-rolled their goals - and flipped when the idle re-ask ceiling
       went 9 -> 25 s in the battery pass (a full fish not eating is right). */
    tank.fish[0].hunger = 8.0f;
    float before = tank.fish[0].hunger; int eaten0 = tank.fish[0].eaten;
    tank_feed(&tank, 220, 3); tank_feed(&tank, 260, 3);
    for (int i = 0; i < (int)(60 / dt); i++) { tank_tick(&tank, dt, advisor_rules); progression_tick(&tank, dt); }
    float after = tank.fish[0].hunger, hmin = 10;
    for (int k = 0; k < tank.n_fish; k++) if (tank.fish[k].hunger < hmin) hmin = tank.fish[k].hunger;
    printf("hunger: keeper drops 6 pellets for hungry %s: %d eaten within a minute, hunger %.1f -> %.1f (fullest fish %.1f)\n",
           tank.fish[0].name, tank.fish[0].eaten - eaten0, before, after, hmin);
    if (tank.fish[0].eaten - eaten0 < 1 || after >= 5.0f) { printf("FAIL: the keeper's feeding didn't fill the hungry fish\n"); return 1; }
    /* a meal should LAST: the fullest fish stays under 7 for at least 4 minutes */
    int idx = 0; for (int k = 0; k < tank.n_fish; k++) if (tank.fish[k].hunger < tank.fish[idx].hunger) idx = k;
    for (int i = 0; i < (int)(4 * 60 / dt); i++) { tank_tick(&tank, dt, advisor_rules); progression_tick(&tank, dt); }
    if (tank.fish[idx].hunger >= 7) { printf("FAIL: %s hungry again (%.1f) 4 min after a meal\n", tank.fish[idx].name, tank.fish[idx].hunger); return 1; }
    printf("hunger: %s still %.1f four minutes on. selftest-hunger ok\n", tank.fish[idx].name, tank.fish[idx].hunger);
    return 0;
}

/* --bench: headless render-cost profile (per-stage microseconds, averaged
 * over 300 frames) for the scenes that decide the device's frame budget:
 * 4 fish with the canopy at nubs vs fully grown, a fouled glass, and the
 * stats card. The Mac is ~10x the ESP32-S3, but the stage RATIOS carry
 * over - this is how the vegetation and card costs were measured. */
#include <time.h>
static int64_t bench_clock_us(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}
static void bench_scene(const char *label, bool card) {
    static uint16_t fb[TANK_W * TANK_H];
    const int N = 300;
    memset(render_prof_us, 0, sizeof render_prof_us);
    int64_t card_us = 0, total_us = 0;
    for (int i = 0; i < N; i++) {
        tank_tick(&tank, 1.0f / 25.0f, advisor_rules);
        tank.night = false;                        /* day palette, shafts on */
        int64_t t0 = bench_clock_us();
        render_tank(&tank, fb, TANK_W);
        int64_t t1 = bench_clock_us();
        if (card) render_stats_card(&tank, 0, fb, TANK_W);
        int64_t t2 = bench_clock_us();
        card_us += t2 - t1; total_us += t2 - t0;
    }
    printf("%-28s total %6.0f us | scene %5.0f shafts %5.0f veg %5.0f fd/bub %5.0f fish %5.0f vig %5.0f algae %5.0f | card %5.0f\n",
           label, (double)total_us / N, (double)render_prof_us[0] / N, (double)render_prof_us[1] / N,
           (double)render_prof_us[2] / N, (double)render_prof_us[3] / N, (double)render_prof_us[4] / N,
           (double)render_prof_us[5] / N, (double)render_prof_us[6] / N, (double)card_us / N);
}
static int bench(void) {
    static uint16_t scene[TANK_W * TANK_H];
    tank_init(&tank, 77);
    tank_new_population(&tank);
    while (tank.n_fish < 4) tank_add_fish(&tank, 0, 1);
    for (int i = 0; i < tank.n_fish; i++) tank.fish[i].stage = STAGE_ADULT;
    tank.tank_ms_bits = 0x1a7;
    render_set_scene_cache(scene);
    render_set_vignette_cache(vig_buf);
    render_set_card_cache(card_buf);
    render_set_dirty_mask(dirty_buf);
    render_clock_us = bench_clock_us;
    for (int b = 0; b < VEG_BEDS; b++) tank_veg_set(&tank, b, VEG_NUB);
    bench_scene("4 fish, canopy at nubs", false);
    for (int b = 0; b < VEG_BEDS; b++) tank_veg_set(&tank, b, 1.0f);
    bench_scene("4 fish, canopy full", false);
    bench_scene("4 fish, canopy full + card", true);
    tank_grow_algae(&tank, 400);
    bench_scene("... + fouled glass", false);
    return 0;
}

int main(int argc, char **argv) {
    for (int a = 1; a < argc; a++)
        if (strcmp(argv[a], "--greedy") == 0) advisor_core_sample = false;
    for (int a = 1; a < argc; a++) {                 /* mode flags may sit anywhere */
        if (strcmp(argv[a], "--snapshot") == 0 && a + 1 < argc)
            return snapshot(argv[a + 1], a + 2 < argc ? atoi(argv[a + 2]) : 20);
        if (strcmp(argv[a], "--selftest") == 0) return selftest();
        if (strcmp(argv[a], "--bench") == 0) return bench();
        if (strcmp(argv[a], "--selftest-hunger") == 0) return selftest_hunger();
        if (strcmp(argv[a], "--selftest-pop") == 0) return selftest_pop();
        if (strcmp(argv[a], "--selftest-sleep") == 0) return selftest_sleep();
        if (strcmp(argv[a], "--selftest-tend") == 0) return selftest_tend();
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
    if (progression_setup_pending()) { setup_begin(&tank); printf("first-run setup: welcome, names, colours (S re-opens it)\n"); }
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
    render_set_vignette_cache(vig_buf);
    render_set_card_cache(card_buf);
    render_set_dirty_mask(dirty_buf);

    last_ms = SDL_GetTicks();
    lv_timer_create(frame_cb, 16, NULL);

    bool fdown = false, ndown = false, ldown = false;
    bool udown = false, mdown = false, mkdown = false, rdown = false, zdown = false, gdown = false, xdown = false, sdown = false;
    uint32_t press_ms = 0; int press_x = 0, press_y = 0;
    float press_fx[N_FISH_MAX] = {0}, press_fy[N_FISH_MAX] = {0};
    while (1) {
        uint32_t wait = lv_timer_handler();
        const Uint8 *k = SDL_GetKeyboardState(NULL);
        int mx, my;
        bool mpress = SDL_GetMouseState(&mx, &my) & SDL_BUTTON(SDL_BUTTON_LEFT);
        uint32_t now_ms = SDL_GetTicks();
        /* mouse -> touch gestures (device: FT3168 does the same job)
         *   press+release < 350 ms, little movement: TAP (on a fish = select its card;
         *                                              card up + empty glass = dismiss;
         *                                              on the surface = feed)
         *   drag down >= 40 px starting near the top: FEED at that x
         *   held > 300 ms: HOLD (finger resting on the glass) */
        if (mpress && !mdown) {
            press_ms = now_ms; press_x = mx; press_y = my;
            for (int i = 0; i < tank.n_fish; i++) { press_fx[i] = tank.fish[i].x; press_fy[i] = tank.fish[i].y; }
        }
        bool setup_up = setup_active();          /* before the touch: BEGIN's release must not become a tank tap */
        if (setup_up) {
            bool birth = setup_is_birth(); int who = setup_fish();
            setup_touch(&tank, (float)mx, (float)my, mpress);   /* taps and the letter wheel, classified in setup.c */
            if (!setup_active()) { printf(birth ? "birth flow done: %s named and saved\n" : "setup done\n", who >= 0 ? tank.fish[who].name : "?"); print_roster(&tank); }
        }
        bool modal = confirm_view || setup_up;
        if (mpress && !modal) tank_touch_drag(&tank, (float)mx, (float)my);   /* stroke -> wipe/slash */
        if (mpress && !modal && now_ms - press_ms > 300 && abs(my - press_y) < 30) tank_touch_hold(&tank, (float)mx, (float)my);
        if (!mpress && mdown) {
            int dx = mx - press_x, dy = my - press_y;
            if (confirm_view) {                    /* the prompt owns the glass: press AND release on one button */
                int h = press_ms > confirm_ms ? render_confirm_hit((float)press_x, (float)press_y) : 0;
                if (h && h == render_confirm_hit((float)mx, (float)my)) {
                    confirm_view = false;
                    if (h > 0) { progression_reset(&tank, SDL_GetTicks() + 7); selected_fish = -1;
                                 printf("RESET: a fresh tank\n"); print_roster(&tank); setup_begin(&tank); }
                    else printf("reset prompt: NO, tank kept\n");
                }
            }
            else if (setup_up) { /* the setup owns the glass: setup_touch took it */ }
            else if (milestones_view) {
                int r = render_milestones_tap(&tank, (float)press_x, (float)press_y);
                if (r == MS_TAP_CLOSE) { milestones_view = false; progression_ack_milestones(&tank); render_milestones_leave(); }
                else if (r == MS_TAP_NONE && render_brightness_row_hit((float)press_x, (float)press_y))
                    sim_bright = sim_bright == 100 ? 60 : sim_bright == 60 ? 30 : 100;   /* the row cycles, the page stays */
                /* MS_TAP_KEPT: a badge / name opened the detail modal, or the modal closed; anything else: nothing */
            }
            else if (now_ms - press_ms < 350 && dx * dx + dy * dy < 24 * 24) {
                /* same hit test as the device: 38 px against the press-time
                   fish snapshot AND the current position, whichever is closer */
                int best = -1; float bd = 38 * 38;
                for (int i = 0; i < tank.n_fish; i++) {
                    float ax = press_fx[i] - press_x, ay = press_fy[i] - press_y;
                    float bx = tank.fish[i].x - press_x, by = tank.fish[i].y - press_y;
                    float d2a = ax * ax + ay * ay, d2b = bx * bx + by * by;
                    float d2 = d2a < d2b ? d2a : d2b;
                    if (d2 < bd) { bd = d2; best = i; }
                }
                if (best >= 0) selected_fish = (best == selected_fish) ? -1 : best;
                else if (selected_fish >= 0) selected_fish = -1;   /* card up: empty-glass tap dismisses, nothing else */
                else tank_touch_tap(&tank, (float)press_x, (float)press_y);
            } else if (press_y < 60 && dy >= 40) tank_feed(&tank, (float)mx, 3);
        }
        mdown = mpress;
        if (confirm_view && now_ms - confirm_ms > CONFIRM_MS) { confirm_view = false; printf("reset prompt: timed out, tank kept\n"); }
        if (k[SDL_SCANCODE_X] && !xdown && !confirm_view) {   /* the keeper's reset prompt (device: hold BOOT + tap) */
            confirm_view = true; confirm_ms = now_ms; selected_fish = -1; milestones_view = false;
            printf("reset prompt: click YES or NO (it gives up after %d s)\n", CONFIRM_MS / 1000);
        }
        xdown = k[SDL_SCANCODE_X];
        if (k[SDL_SCANCODE_S] && !sdown) {                    /* the first-run flow, on cue */
            if (setup_active()) { setup_cancel(&tank); printf("setup: closed (still owed if it was pending)\n"); }
            else { setup_begin(&tank); selected_fish = -1; milestones_view = false; printf("setup: welcome page (click through; BEGIN saves)\n"); }
        }
        sdown = k[SDL_SCANCODE_S];
        if (k[SDL_SCANCODE_U] && !udown) { ui_visible = !ui_visible; }
        udown = k[SDL_SCANCODE_U];
        if (k[SDL_SCANCODE_M] && !mkdown) {
            milestones_view = !milestones_view;
            if (!milestones_view) { progression_ack_milestones(&tank); render_milestones_leave(); }
        }
        mkdown = k[SDL_SCANCODE_M];
        if (k[SDL_SCANCODE_R] && !rdown) { progression_force_arrival(&tank); print_roster(&tank); }
        rdown = k[SDL_SCANCODE_R];
        if (k[SDL_SCANCODE_Z] && !zdown) {         /* jump through a night of device sleep */
            tank_tick_sleep(&tank, 7 * 3600);
            printf("slept 7 h: hunger now");
            for (int i = 0; i < tank.n_fish; i++) printf(" %.1f", tank.fish[i].hunger);
            printf("\n");
        }
        zdown = k[SDL_SCANCODE_Z];
        if (k[SDL_SCANCODE_G] && !gdown) {         /* demo the upkeep chores at once */
            for (int b = 0; b < VEG_BEDS; b++)
                tank_veg_set(&tank, b, tank.veg_growth[b] > 0.99f ? VEG_NUB
                                   : fminf(1, tank.veg_growth[b] + 0.30f));
            tank_grow_algae(&tank, 80);
            printf("grew: canopy %.2f/%.2f/%.2f + algae (swipe sideways through a canopy to trim; drag to wipe; G cycles)\n",
                   tank.veg_growth[0], tank.veg_growth[1], tank.veg_growth[2]);
        }
        gdown = k[SDL_SCANCODE_G];
        if (k[SDL_SCANCODE_Q] || k[SDL_SCANCODE_ESCAPE]) { progression_save(&tank); break; }
        if (k[SDL_SCANCODE_F] && !fdown) tank_feed(&tank, (float)mx, 3);
        if (k[SDL_SCANCODE_N] && !ndown) tank_toggle_light(&tank);
        if (k[SDL_SCANCODE_A]) tank_light_auto(&tank);
        if (k[SDL_SCANCODE_L] && !ldown && llm_available) {
            llm_active = !llm_active;
            printf("brain: %s\n", llm_active ? "LLM (14M student)" : "rules");
        }
        fdown = k[SDL_SCANCODE_F]; ndown = k[SDL_SCANCODE_N];
        ldown = k[SDL_SCANCODE_L];
        SDL_Delay(wait < 5 ? 5 : (wait > 16 ? 16 : wait));
    }
    return 0;
}
