/* setup.c — first-run setup flow (see setup.h). */
#include "setup.h"
#include "render.h"
#include "progression.h"
#include <string.h>
#include <stdio.h>

/* the reset prompt's palette: ink panel, teal edge, calm / lit buttons */
#define C_PANEL  0x04141a
#define C_EDGE   0x9fd8e2
#define C_INNER  0x1c2f36
#define C_DIM    0x2a3f45
#define C_KEY    0x0e2229
#define C_TEXT   0xffffff
#define C_CAPT   0x9fd8e2
#define C_GO     0x155e58      /* BEGIN: the teal of a charging battery */
#define C_GO_E   0x38dcc7

static bool s_active;
static int  s_page;
static int  s_slot;            /* name pages: the slot the wheel turns */
/* the finger (setup_touch) */
static bool  s_down;
static float s_px, s_py;       /* press point */
static float s_ly;             /* last y, for the wheel */
static float s_acc;            /* vertical travel toward the next step */
static bool  s_spun;           /* this press has turned the wheel: no tap on release */

static int page_fish(void) { return (s_page == SETUP_PG_NAME_A || s_page == SETUP_PG_LOOK_A) ? 0 : 1; }
static bool page_is_name(void) { return s_page == SETUP_PG_NAME_A || s_page == SETUP_PG_NAME_B; }
static bool page_is_look(void) { return s_page == SETUP_PG_LOOK_A || s_page == SETUP_PG_LOOK_B; }
static bool nav_on_top(void) { return page_is_name() || page_is_look(); }

void setup_begin(tank_t *t) {
    if (t->n_fish < 2) { progression_setup_done(t); s_active = false; return; }   /* nothing to name */
    s_active = true; s_page = SETUP_PG_WELCOME; s_slot = 0; s_down = false;
}
bool setup_active(void) { return s_active; }
static void stage(tank_t *t) {                          /* who is on stage, and where */
    if (s_active && (page_is_name() || page_is_look())) {
        t->stage_fish = (int8_t)page_fish(); t->stage_x = SETUP_STAGE_X;
        t->stage_y = page_is_name() ? SETUP_STAGE_NAME_Y : SETUP_STAGE_LOOK_Y;
    } else t->stage_fish = -1;
}
void setup_cancel(tank_t *t) { s_active = false; stage(t); }
int  setup_page(void)   { return s_page; }
int  setup_slot(void)   { return s_slot; }

const char *setup_hit_name(int id) {
    static char buf[12];
    if (id == SETUP_HIT_NEXT) return "NEXT";
    if (id == SETUP_HIT_BACK) return "BACK";
    if (id == SETUP_HIT_UP)   return "up";
    if (id == SETUP_HIT_DOWN) return "down";
    if (id >= SETUP_HIT_SLOT0 && id < SETUP_HIT_SLOT0 + FISH_NAME_MAX) { snprintf(buf, sizeof buf, "slot %d", id - SETUP_HIT_SLOT0); return buf; }
    if (id >= SETUP_HIT_BODY0 && id < SETUP_HIT_BODY0 + LOOK_N) { snprintf(buf, sizeof buf, "body %d", id - SETUP_HIT_BODY0); return buf; }
    return "nothing";
}

/* ---- the letter wheel over a fish's name ----
 * A slot holds one of 27 values: blank, A..Z. The name string carries the
 * slots up to the last letter (blanks inside it are spaces); NEXT / BACK
 * tidy it - trailing blanks dropped, inner ones closed up, nothing left =
 * the preset's name again. */
static int slot_val(const fish_t *f, int i) {           /* 0 = blank, 1..26 = A..Z */
    if (i >= (int)strlen(f->name)) return 0;
    char ch = f->name[i];
    if (ch >= 'a' && ch <= 'z') ch -= 'a' - 'A';
    return ch >= 'A' && ch <= 'Z' ? ch - 'A' + 1 : 0;
}
static void slot_set(fish_t *f, int i, int v) {
    int n = (int)strlen(f->name);
    if (v == 0) {
        if (i >= n) return;
        f->name[i] = ' ';
        while (n > 0 && f->name[n - 1] == ' ') f->name[--n] = 0;   /* a blank at the end shortens */
        return;
    }
    while (n < i) f->name[n++] = ' ';                               /* (unreachable: slots snap to <= len) */
    f->name[i] = (char)('A' + v - 1);
    if (i >= n) f->name[i + 1] = 0;
}
static int name_len(const fish_t *f) { return (int)strlen(f->name); }
static void pick_slot(const fish_t *f, int i) {         /* the first blank is the last pickable slot */
    int n = name_len(f);
    if (i > n) i = n;
    if (i > FISH_NAME_MAX - 1) i = FISH_NAME_MAX - 1;
    if (i < 0) i = 0;
    s_slot = i;
}
static void spin(fish_t *f, int dir) {
    if (name_len(f) < s_slot) pick_slot(f, s_slot);
    slot_set(f, s_slot, (slot_val(f, s_slot) + 27 + dir) % 27);
}
static void tidy_name(tank_t *t, int fish) {
    fish_t *f = &t->fish[fish];
    char out[FISH_NAME_MAX + 1]; int n = 0;
    for (const char *p = f->name; *p; p++) if (*p != ' ' && n < FISH_NAME_MAX) out[n++] = *p;
    out[n] = 0;
    tank_set_name(t, fish, out);                        /* empty = the preset's */
}

static bool in_box(float x, float y, int bx, int by, int bw, int bh, int m) {
    return x >= bx - m && x < bx + bw + m && y >= by - m && y < by + bh + m;
}
static int nearest_slot(float x) {
    int i = (int)((x - SETUP_SLOT_X + (SETUP_SLOT_PX - SETUP_SLOT_W) * 0.5f) / SETUP_SLOT_PX);
    return i < 0 ? 0 : i >= FISH_NAME_MAX ? FISH_NAME_MAX - 1 : i;
}
int setup_hit(float x, float y) {
    if (!s_active) return 0;
    const int m = 10;                                   /* a fingertip's slop, as on the reset prompt */
    if (s_page == SETUP_PG_WELCOME)
        return in_box(x, y, SETUP_MID_X, SETUP_BTN_Y, SETUP_BTN_W, SETUP_BTN_H, m) ? SETUP_HIT_NEXT : 0;
    if (nav_on_top()) {
        if (in_box(x, y, SETUP_TOP_NEXT_X, SETUP_TOP_BTN_Y, SETUP_TOP_BTN_W, SETUP_BTN_H, m)) return SETUP_HIT_NEXT;
        if (in_box(x, y, SETUP_TOP_BACK_X, SETUP_TOP_BTN_Y, SETUP_TOP_BTN_W, SETUP_BTN_H, m)) return SETUP_HIT_BACK;
    } else {
        if (in_box(x, y, SETUP_NEXT_X, SETUP_BTN_Y, SETUP_BTN_W, SETUP_BTN_H, m)) return SETUP_HIT_NEXT;
        if (in_box(x, y, SETUP_BACK_X, SETUP_BTN_Y, SETUP_BTN_W, SETUP_BTN_H, m)) return SETUP_HIT_BACK;
    }
    if (page_is_name()) {
        int rw = (FISH_NAME_MAX - 1) * SETUP_SLOT_PX + SETUP_SLOT_W;
        if (x < SETUP_SLOT_X - 24 || x >= SETUP_SLOT_X + rw + 24) return 0;
        /* the row band picks a slot; the bands above and below are the
           chevrons - over the active slot and its neighbours, a hand wide */
        if (y >= SETUP_SLOT_Y - 16 && y < SETUP_SLOT_Y + SETUP_SLOT_H + 20) return SETUP_HIT_SLOT0 + nearest_slot(x);
        float cx = SETUP_SLOT_X + s_slot * SETUP_SLOT_PX + SETUP_SLOT_W * 0.5f;
        if (x < cx - 66 || x > cx + 66) return 0;
        if (y >= SETUP_SLOT_Y - 16 - 70 && y < SETUP_SLOT_Y - 16) return SETUP_HIT_UP;
        if (y >= SETUP_SLOT_Y + SETUP_SLOT_H + 20 && y < SETUP_SLOT_Y + SETUP_SLOT_H + 20 + 70) return SETUP_HIT_DOWN;
        return 0;
    }
    if (page_is_look()) {
        /* the swatch row, a tall band (40 px above, 40 below - fingers land
           low); the nearest swatch along x, no dead space */
        int rw = (SETUP_SW_N - 1) * SETUP_SW_PX + SETUP_SW_W;
        if (!in_box(x, y, SETUP_SW_X, SETUP_SW_Y, rw, SETUP_SW_H, 40) || y >= SETUP_ACC_Y) return 0;
        int i = (int)((x - SETUP_SW_X + (SETUP_SW_PX - SETUP_SW_W) * 0.5f) / SETUP_SW_PX);
        if (i < 0) i = 0;
        if (i >= SETUP_SW_N) i = SETUP_SW_N - 1;
        return SETUP_HIT_BODY0 + i;
    }
    return 0;
}

void setup_activate(tank_t *t, int id) {
    if (!s_active || !id) return;
    fish_t *f = &t->fish[page_fish()];
    if (id == SETUP_HIT_NEXT) {
        if (page_is_name()) tidy_name(t, page_fish());
        if (s_page == SETUP_PG_CARE) { s_active = false; stage(t); progression_setup_done(t); return; }
        s_page++; s_slot = 0; stage(t);
        return;
    }
    if (id == SETUP_HIT_BACK) {
        if (page_is_name()) tidy_name(t, page_fish());
        if (s_page > SETUP_PG_WELCOME) s_page--;
        s_slot = 0; stage(t);
        return;
    }
    if (page_is_name()) {
        if (id >= SETUP_HIT_SLOT0 && id < SETUP_HIT_SLOT0 + FISH_NAME_MAX) pick_slot(f, id - SETUP_HIT_SLOT0);
        else if (id == SETUP_HIT_UP)   spin(f, +1);
        else if (id == SETUP_HIT_DOWN) spin(f, -1);
        return;
    }
    if (page_is_look() && id >= SETUP_HIT_BODY0 && id < SETUP_HIT_BODY0 + LOOK_N)
        tank_set_look(t, page_fish(), LOOK_BODY[id - SETUP_HIT_BODY0], 0);   /* the accent stays its secret */
}

void setup_touch(tank_t *t, float x, float y, bool down) {
    stage(t);                                           /* every frame: the tank keeps the fish on stage */
    if (!s_active) { s_down = down; return; }
    if (down && !s_down) {                              /* press */
        s_px = x; s_py = y; s_ly = y; s_acc = 0; s_spun = false;
        int h = setup_hit(x, y);
        if (page_is_name() && h >= SETUP_HIT_SLOT0 && h < SETUP_HIT_SLOT0 + FISH_NAME_MAX)
            pick_slot(&t->fish[page_fish()], h - SETUP_HIT_SLOT0);   /* picked on touch: the drag turns it */
    } else if (down && page_is_name()) {                /* the wheel: vertical travel spins the letter */
        int h0 = setup_hit(s_px, s_py);
        if (h0 >= SETUP_HIT_SLOT0 && h0 < SETUP_HIT_SLOT0 + FISH_NAME_MAX) {
            s_acc += y - s_ly;
            while (s_acc <= -SETUP_SPIN_PX) { spin(&t->fish[page_fish()], +1); s_acc += SETUP_SPIN_PX; s_spun = true; }   /* up = next letter */
            while (s_acc >=  SETUP_SPIN_PX) { spin(&t->fish[page_fish()], -1); s_acc -= SETUP_SPIN_PX; s_spun = true; }
        }
        s_ly = y;
    } else if (!down && s_down) {                       /* release: a tap, unless the wheel turned */
        if (!s_spun) {
            int h = setup_hit(s_px, s_py);
            if (h && h == setup_hit(x, y)) setup_activate(t, h);
        }
    }
    s_down = down;
}

/* ---- drawing ---- */
static void text_c(uint16_t *fb, int stride, int cx, int y, int scale, uint32_t rgb, const char *s) {
    render_text(fb, stride, cx - render_text_w(s, scale) / 2, y, scale, rgb, s);
}
static void lines_c(uint16_t *fb, int stride, int y, int pitch, uint32_t rgb, const char *const *ls, int n) {
    for (int i = 0; i < n; i++) text_c(fb, stride, SETUP_X + SETUP_W / 2, y + i * pitch, 2, rgb, ls[i]);
}
static void nav(uint16_t *fb, int stride, bool top, const char *next_label, bool go) {
    int y = top ? SETUP_TOP_BTN_Y : SETUP_BTN_Y, w = top ? SETUP_TOP_BTN_W : SETUP_BTN_W;
    render_button(fb, stride, top ? SETUP_TOP_BACK_X : SETUP_BACK_X, y, w, SETUP_BTN_H, C_KEY, C_DIM, "BACK", 2);
    render_button(fb, stride, top ? SETUP_TOP_NEXT_X : SETUP_NEXT_X, y, w, SETUP_BTN_H,
                  go ? C_GO : C_INNER, go ? C_GO_E : C_EDGE, next_label, 2);
}
/* page dots along the foot: where the keeper is in the flow */
static void dots(uint16_t *fb, int stride, int y) {
    int n = SETUP_PG_N, w = n * 10 - 4, x0 = (TANK_W - w) / 2;
    for (int i = 0; i < n; i++) render_rect(fb, stride, x0 + i * 10, y, 6, 3, i == s_page ? C_EDGE : C_DIM);
}
static uint32_t dim(uint32_t c, int pct) {              /* c toward the ink, pct% of it left */
    return ((c >> 16 & 255) * pct / 100) << 16 | ((c >> 8 & 255) * pct / 100) << 8 | (c & 255) * pct / 100;
}
/* a chevron of 4x4 blocks, `up` pointing up, apex at (cx, y) */
static void chevron(uint16_t *fb, int stride, int cx, int y, bool up, uint32_t rgb) {
    for (int i = 0; i < 5; i++) {
        int yy = up ? y + i * 4 : y - i * 4;
        render_rect(fb, stride, cx - 4 - i * 4, yy, 4, 4, rgb);
        render_rect(fb, stride, cx + i * 4, yy, 4, 4, rgb);
    }
}
static void panel(uint16_t *fb, int stride) {
    render_rect(fb, stride, SETUP_X, SETUP_Y, SETUP_W, SETUP_H, C_PANEL);
    render_rect_edge(fb, stride, SETUP_X, SETUP_Y, SETUP_W, SETUP_H, C_EDGE);
    render_rect_edge(fb, stride, SETUP_X + 1, SETUP_Y + 1, SETUP_W - 2, SETUP_H - 2, C_INNER);
}

void render_setup(const tank_t *t, uint16_t *fb, int stride, float clock) {
    if (!s_active) return;
    const int CX = TANK_W / 2;
    if (s_page == SETUP_PG_WELCOME) {
        panel(fb, stride);
        text_c(fb, stride, CX, SETUP_Y + 36, 3, C_TEXT, "WELCOME");
        static const char *const ls[] = {
            "TWO FRY HAVE MOVED IN.",
            "THEY EAT, PLAY, REST AND",
            "GROW UP WHILE YOU WATCH.",
            "EACH ONE THINKS FOR ITSELF.",
            "FIRST, LET'S MEET THEM.",
        };
        lines_c(fb, stride, SETUP_Y + 96, 24, C_CAPT, ls, 5);
        render_button(fb, stride, SETUP_MID_X, SETUP_BTN_Y, SETUP_BTN_W, SETUP_BTN_H, C_INNER, C_EDGE, "NEXT", 2);
        dots(fb, stride, SETUP_Y + SETUP_H - 8);
    } else if (page_is_name()) {
        /* straight on the tank: the fish being named wears a ring in its own
           colour, its name spans the middle in that colour, the active slot
           bright with the chevrons, the rest dimmed; slots past the first
           blank are just faint underlines */
        const fish_t *f = &t->fish[page_fish()];
        render_ring(fb, stride, f->x, f->y, 17 * f->size + 6, f->color);
        render_rect_blend(fb, stride, 0, SETUP_SLOT_Y - SETUP_ARROW_GAP - 2, TANK_W, SETUP_SLOT_H + 2 * SETUP_ARROW_GAP + 36, C_PANEL, 150);
        text_c(fb, stride, CX, SETUP_Y + 7, 2, C_CAPT, page_fish() == 0 ? "NAME THE FIRST FISH" : "NAME THE SECOND FISH");
        nav(fb, stride, true, "NEXT", false);
        int n = name_len(f);
        if (s_slot > n) s_slot = n;
        for (int i = 0; i < FISH_NAME_MAX; i++) {
            int x = SETUP_SLOT_X + i * SETUP_SLOT_PX;
            bool on = i == s_slot, reach = i <= n;
            uint32_t col = on ? f->color : reach ? dim(f->color, 55) : C_DIM;
            int v = slot_val(f, i);
            if (v) { char ch[2] = { (char)('A' + v - 1), 0 }; render_text(fb, stride, x, SETUP_SLOT_Y, SETUP_SLOT_SCALE, col, ch); }
            render_rect(fb, stride, x, SETUP_SLOT_Y + SETUP_SLOT_H + 8, SETUP_SLOT_W, 4,
                        on && !v && ((int)(clock * 2) & 1) ? C_TEXT : col);
            if (on) {
                chevron(fb, stride, x + SETUP_SLOT_W / 2, SETUP_SLOT_Y - SETUP_ARROW_GAP, true, C_EDGE);
                chevron(fb, stride, x + SETUP_SLOT_W / 2, SETUP_SLOT_Y + SETUP_SLOT_H + SETUP_ARROW_GAP + 4, false, C_EDGE);
            }
        }
        text_c(fb, stride, CX, SETUP_SLOT_Y + SETUP_SLOT_H + 96, 2, C_CAPT, "SWIPE A LETTER UP OR DOWN");
        dots(fb, stride, TANK_H - 14);
    } else if (page_is_look()) {
        /* straight on the tank again: the ringed FRY is the preview (no
           grown-up look - that is the surprise); a row of body swatches; the
           accent a "?" that its first growth spurt answers */
        const fish_t *f = &t->fish[page_fish()];
        render_ring(fb, stride, f->x, f->y, 17 * f->size + 6, f->color);
        render_rect_blend(fb, stride, 0, SETUP_SW_Y - 30, TANK_W, SETUP_ACC_Y + SETUP_SW_H + 16 - (SETUP_SW_Y - 30), C_PANEL, 110);
        char cap[FISH_NAME_MAX + 16]; snprintf(cap, sizeof cap, "A COLOR FOR %s", f->name);
        text_c(fb, stride, CX, SETUP_Y + 7, 2, C_CAPT, cap);
        nav(fb, stride, true, "NEXT", false);
        render_text(fb, stride, SETUP_SW_X + 2, SETUP_SW_Y - 20, 2, C_CAPT, "BODY");
        for (int i = 0; i < SETUP_SW_N; i++) {
            int x = SETUP_SW_X + i * SETUP_SW_PX;
            bool on = LOOK_BODY[i] == f->color;
            render_rect(fb, stride, x, SETUP_SW_Y, SETUP_SW_W, SETUP_SW_H, LOOK_BODY[i]);
            render_rect_edge(fb, stride, x, SETUP_SW_Y, SETUP_SW_W, SETUP_SW_H, on ? C_TEXT : C_DIM);
            if (on) render_rect_edge(fb, stride, x + 1, SETUP_SW_Y + 1, SETUP_SW_W - 2, SETUP_SW_H - 2, C_TEXT);
        }
        render_text(fb, stride, SETUP_SW_X + 2, SETUP_ACC_Y - 20, 2, C_CAPT, "ACCENT");
        render_rect(fb, stride, SETUP_SW_X, SETUP_ACC_Y, SETUP_SW_W, SETUP_SW_H, C_INNER);
        render_rect_edge(fb, stride, SETUP_SW_X, SETUP_ACC_Y, SETUP_SW_W, SETUP_SW_H, C_DIM);
        render_text(fb, stride, SETUP_SW_X + (SETUP_SW_W - 20) / 2, SETUP_ACC_Y + (SETUP_SW_H - 28) / 2, 4, C_CAPT, "?");
        render_text(fb, stride, SETUP_SW_X + SETUP_SW_W + 16, SETUP_ACC_Y + 12, 2, C_CAPT, "ITS MARKINGS COME IN");
        render_text(fb, stride, SETUP_SW_X + SETUP_SW_W + 16, SETUP_ACC_Y + 32, 2, C_CAPT, "AS IT GROWS UP");
        dots(fb, stride, TANK_H - 14);
    } else {                                                        /* SETUP_PG_CARE */
        panel(fb, stride);
        text_c(fb, stride, CX, SETUP_Y + 22, 3, C_TEXT, "CARING FOR THEM");
        static const char *const ls[] = {
            "TAP THE SURFACE TO FEED",
            "HOLD A FINGER AND THEY VISIT",
            "TAP A FISH TO CHECK ON IT",
            "DRAG THE GLASS TO WIPE ALGAE",
            "SWIPE ACROSS GRASS TO TRIM",
            "TWO TAPS FLIP THE LIGHT",
            "THEY CHOOSE. YOU CARE.",
        };
        lines_c(fb, stride, SETUP_Y + 66, 24, C_CAPT, ls, 7);
        nav(fb, stride, false, "BEGIN", true);
        dots(fb, stride, SETUP_Y + SETUP_H - 8);
    }
}
