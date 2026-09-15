/* touch_port_ft3168.c — FT3168 capacitive touch (FT5x06 register family) ->
 * tank_touch_hold / tank_touch_tap, with the same gesture timing as the sim's
 * mouse: press+release < 350 ms with < 24 px displacement = tap (fingertips
 * roll and this panel is 322 ppi); held > 300 ms = hold; a drag down from
 * the top edge = feed at that x; every touched frame streams to
 * tank_touch_drag (a moving stroke wipes algae; a horizontal slash through
 * a canopy trims it). Fish taps hit-test 38 px against the press-time fish
 * snapshot AND the current position - fish move during a tap. While the stats
 * card is up, a tap anywhere on empty glass dismisses it (hunting the same
 * fish again to close it was the old, cumbersome way) and does nothing else.
 * Coordinates are mapped from the portrait panel to the landscape tank. */
#include "touch_port.h"
#include "board_pins.h"
#include "tank.h"
#include "render.h"
#include "setup.h"
#include "notice.h"
#include "audio_port.h"
#include "progression.h"
#include "esp_lcd_touch_ft5x06.h"
#include "esp_lcd_touch_cst816s.h"
#include "esp_lcd_panel_io.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "driver/i2c_master.h"
#include <math.h>

static const char *TAG = "touch";
static esp_lcd_touch_handle_t s_tp;
static bool s_down; static int64_t s_press_us; static float s_px, s_py;
static float s_lx, s_ly;                          /* LAST touched position (release classification) */
static float s_fx[N_FISH_MAX], s_fy[N_FISH_MAX];  /* fish positions at press time */
static int s_sel = -1; static int64_t s_sel_us;   /* tapped fish -> stats card */
static bool s_ms;                                 /* milestones page up (its CLOSE button ends it) */
static bool s_cf; static int64_t s_cf_us; static int s_cf_ans;   /* reset confirm prompt */
static bool s_bright_tap;                         /* milestones page: brightness row tapped */
#define CONFIRM_TIMEOUT_US (20LL * 1000000)
static bool s_inverted;                           /* screen 180-flipped: mirror into tank space */
/* Fingers land a little BELOW where the eye aims - the pad rolls onto the
 * glass under the fingertip (phones shift their hit targets down for the
 * same reason; Strato saw it on the swatch rows, 2026-09-13). Reported
 * points move UP by this many px in displayed space; director `touch bias
 * <px>` tunes it live. */
static int s_bias_y = 10;
void touch_port_set_bias(int px) { s_bias_y = px; }
int  touch_port_bias(void) { return s_bias_y; }

void touch_port_set_inverted(bool inverted) { s_inverted = inverted; }
extern i2c_master_bus_handle_t board_i2c_bus(void);
extern bool board_is_v2(void);

bool touch_port_init(void) {
    esp_lcd_panel_io_handle_t io;
    bool v2 = board_is_v2();
    esp_lcd_panel_io_i2c_config_t io_cfg = v2 ? (esp_lcd_panel_io_i2c_config_t)ESP_LCD_TOUCH_IO_I2C_CST816S_CONFIG()
                                              : (esp_lcd_panel_io_i2c_config_t)ESP_LCD_TOUCH_IO_I2C_FT5x06_CONFIG();
    io_cfg.dev_addr = v2 ? I2C_ADDR_CST816 : I2C_ADDR_FT3168; io_cfg.scl_speed_hz = 400000;
    if (esp_lcd_new_panel_io_i2c(board_i2c_bus(), &io_cfg, &io) != ESP_OK) { ESP_LOGW(TAG, "no touch io"); return false; }
    esp_lcd_touch_config_t tp_cfg = { .x_max = PANEL_W, .y_max = PANEL_H, .rst_gpio_num = -1, .int_gpio_num = -1,
        .levels = { .reset = 0, .interrupt = 0 }, .flags = { .swap_xy = 0, .mirror_x = 0, .mirror_y = 0 } };
    esp_err_t err = v2 ? esp_lcd_touch_new_i2c_cst816s(io, &tp_cfg, &s_tp)
                       : esp_lcd_touch_new_i2c_ft5x06(io, &tp_cfg, &s_tp);
    if (err != ESP_OK) { ESP_LOGW(TAG, "no %s", v2 ? "CST816" : "FT3168"); return false; }
    ESP_LOGI(TAG, "%s ready", v2 ? "CST816" : "FT3168");
    return true;
}

/* call every frame from the tank task */
void touch_port_poll(tank_t *t) {
    int64_t now = esp_timer_get_time();
    if (s_cf && now - s_cf_us > CONFIRM_TIMEOUT_US) touch_port_confirm_answer(-1);   /* nobody answered: keep the tank */
    if (!s_tp) return;
    uint16_t x[1], y[1], st[1]; uint8_t n = 0;
    esp_lcd_touch_read_data(s_tp);
    bool touched = esp_lcd_touch_get_coordinates(s_tp, x, y, st, &n, 1) && n > 0;
    /* portrait panel (px,py) -> landscape tank (tx,ty): tx = TANK_W-1-py, ty = px;
     * flipped screen: mirror both, so downstream gestures live in displayed space */
    float tx = touched ? (s_inverted ? (float)y[0] : (float)(TANK_W - 1 - y[0])) : s_lx;
    float ty = touched ? (s_inverted ? (float)(TANK_H - 1 - x[0]) : (float)x[0]) - s_bias_y : s_ly;
    if (touched && ty < 0) ty = 0;
    if (touched && !s_down) {
        audio_port_prewarm();                   /* the release's cue plays warm */
        s_press_us = now; s_px = tx; s_py = ty;
        /* snapshot the school: the user aims at where a fish WAS - by release
           a darting fish has moved and the finger hid it the whole time */
        for (int i = 0; i < t->n_fish && i < N_FISH_MAX; i++) { s_fx[i] = t->fish[i].x; s_fy[i] = t->fish[i].y; }
    }
    bool su = setup_active();                                /* before the touch: BEGIN's release is not a tank tap */
    if (su && !s_cf) {
        bool birth = setup_is_birth(); int who = setup_fish();
        setup_touch(t, tx, ty, touched);                     /* taps and the letter wheel, classified in setup.c */
        if (!setup_active()) {
            if (birth) ESP_LOGI(TAG, "birth flow done: %s named and saved", who >= 0 && who < t->n_fish ? t->fish[who].name : "?");
            else ESP_LOGI(TAG, "setup done: %s + %s", t->fish[0].name, t->fish[1].name);
        }
    }
    bool modal = s_ms || s_cf || su;                         /* a page or a prompt owns the glass */
    if (touched) { s_lx = tx; s_ly = ty; if (!modal) tank_touch_drag(t, tx, ty); }  /* stroke = wipe/slash */
    if (touched && !modal && now - s_press_us > 300000 && fabsf(ty - s_py) < 30) tank_touch_hold(t, tx, ty);
    if (!touched && s_down) {
        /* release: classify with the LAST touched position (the old code fell
           back to the PRESS position here, so dx/dy were always 0 - every
           quick swipe read as a tap and the drag-feed could never fire) */
        float dx = s_lx - s_px, dy = s_ly - s_py;
        if (s_cf) {                     /* the prompt owns the glass: a press AND release on the
                                           same button answers it, nothing else counts - not
                                           even the tap that opened it (it began before) */
            int h = s_press_us > s_cf_us ? render_confirm_hit(s_px, s_py) : 0;
            if (h && h == render_confirm_hit(s_lx, s_ly)) touch_port_confirm_answer(h);
            goto released;
        }
        if (su) {                       /* the setup had the glass (setup_touch above); just the log:
                                           where the finger landed vs what it hit, in case this panel
                                           reports fingers offset from where they feel */
            ESP_LOGI(TAG, "setup touch press %.0f,%.0f release %.0f,%.0f -> %s", s_px, s_py, s_lx, s_ly,
                     setup_hit_name(setup_active() ? setup_hit(s_px, s_py) : 0));
            s_sel = -1; goto released;
        }
        if (now - s_press_us < 350000 && dx * dx + dy * dy < 24 * 24) {
            if (notice_current()) { notice_dismiss(); ESP_LOGI(TAG, "tap closed the announcement"); goto released; }
            if (s_ms) {                                             /* the page: badges open a modal, the CLOSE
                                                                       button ends it, the brightness row cycles */
                int r = render_milestones_tap(t, s_px, s_py);     /* CLOSE button / detail modal / nothing */
                bool row = r == MS_TAP_NONE && render_brightness_row_hit(s_px, s_py);
                ESP_LOGI(TAG, "page tap at %.0f,%.0f (release %.0f,%.0f) -> %s", s_px, s_py, s_lx, s_ly,
                         r == MS_TAP_CLOSE ? "CLOSE" : r == MS_TAP_KEPT ? "detail" : row ? "brightness row" : "nothing");
                if (row) { s_bright_tap = true; goto released; }
                if (r != MS_TAP_CLOSE) goto released;              /* only the button leaves the page */
                s_ms = false; s_sel = -1;
                progression_ack_milestones(t); render_milestones_leave();   /* everything shown is now "seen" */
                goto released;
            }
            if (s_sel >= 0 && s_px >= RENDER_CARD_X && s_px < RENDER_CARD_X + RENDER_CARD_W &&
                s_py >= RENDER_CARD_Y && s_py < RENDER_CARD_Y + RENDER_CARD_H) {
                s_ms = true; goto released;                          /* a tap ON the card = milestones page */
            }
            /* fish first; only an empty tap reaches the water. 38 px radius
               (a fingertip on this 322 ppi panel covers ~60 px) against BOTH
               the press-time snapshot and the current position - whichever is
               closer - so a fish that moved mid-tap still registers. */
            int best = -1; float bd = 38 * 38;
            for (int i = 0; i < t->n_fish; i++) {
                float ax = s_fx[i] - s_px, ay = s_fy[i] - s_py;
                float bx = t->fish[i].x - s_px, by = t->fish[i].y - s_py;
                float d2a = ax * ax + ay * ay, d2b = bx * bx + by * by;
                float d2 = d2a < d2b ? d2a : d2b;
                if (d2 < bd) { bd = d2; best = i; }
            }
            if (best >= 0) { s_sel = (best == s_sel) ? -1 : best; s_sel_us = now; }
            else if (s_sel >= 0) s_sel = -1;   /* card up: a tap on empty glass just
                                                  dismisses it - it is NOT a tank tap
                                                  (no feed, no light-toggle burst) */
            else tank_touch_tap(t, s_px, s_py);
        }
        else if (!s_ms && s_py < 60 && dy >= 40) tank_feed(t, s_lx, 3);  /* drag down from the top = feed */
    }
released:
    s_down = touched;
    if (s_sel >= t->n_fish) s_sel = -1;                          /* fresh tank / save load */
    if (s_sel >= 0 && now - s_sel_us > 10 * 1000000) s_sel = -1; /* auto-dismiss */
}

int touch_port_selected(void) { return s_sel; }
bool touch_port_milestones(void) { return s_ms; }
void touch_port_show_milestones(bool on) { if (s_ms && !on) render_milestones_leave(); s_ms = on; }
void touch_port_dismiss(void) { s_sel = -1; if (s_ms) render_milestones_leave(); s_ms = false; }

/* ---- reset confirm prompt ---- */
void touch_port_confirm_open(void) {
    s_cf = true; s_cf_us = esp_timer_get_time(); s_cf_ans = 0;
    s_sel = -1; s_ms = false;                     /* it replaces the card / the page */
    ESP_LOGI(TAG, "reset prompt up (YES / NO on the glass; NO by itself in %d s)", (int)(CONFIRM_TIMEOUT_US / 1000000));
}
bool touch_port_confirm_answer(int ans) {
    if (!s_cf) return false;
    s_cf = false; s_cf_ans = ans > 0 ? 1 : -1;
    return true;
}
bool  touch_port_confirm_up(void)   { return s_cf; }
float touch_port_confirm_frac(void) {
    if (!s_cf) return 0;
    float f = 1.0f - (esp_timer_get_time() - s_cf_us) / (float)CONFIRM_TIMEOUT_US;
    return f < 0 ? 0 : f;
}
int  touch_port_confirm_take(void)  { int a = s_cf_ans; s_cf_ans = 0; return a; }
bool touch_port_pressed_since(int64_t us) { return s_down && s_press_us > us; }
bool touch_port_take_brightness_tap(void) { bool b = s_bright_tap; s_bright_tap = false; return b; }
