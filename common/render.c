/* render.c — porthole look: deep gradient water, AMOLED-black floor, procedural
 * fish (body polygon + animated tail + earned markings), bubbles, reef fronds
 * that grow with the tank's milestones, roaming shadow. Everything is drawn
 * into a bare RGB565 buffer; night dims the palette. */
#include "render.h"
#include <math.h>
#include <string.h>

#define TAU 6.2831853f

typedef struct { uint16_t *fb; int stride; float dim; } ctx_t;

static uint16_t rgb565(uint32_t rgb, float dim) {
    uint32_t r = (uint32_t)(((rgb >> 16) & 255) * dim);
    uint32_t g = (uint32_t)(((rgb >> 8) & 255) * dim);
    uint32_t b = (uint32_t)((rgb & 255) * dim);
    return (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

static void px(ctx_t *c, int x, int y, uint16_t col) {
    if ((unsigned)x < TANK_W && (unsigned)y < TANK_H)
        c->fb[y * c->stride + x] = col;
}

/* alpha 0..255 blend onto existing pixel */
static void px_blend(ctx_t *c, int x, int y, uint32_t rgb, int a) {
    if ((unsigned)x >= TANK_W || (unsigned)y >= TANK_H) return;
    uint16_t *p = &c->fb[y * c->stride + x];
    int dr = (*p >> 11) << 3, dg = ((*p >> 5) & 63) << 2, db = (*p & 31) << 3;
    int sr = (int)(((rgb >> 16) & 255) * c->dim);
    int sg = (int)(((rgb >> 8) & 255) * c->dim);
    int sb = (int)((rgb & 255) * c->dim);
    int r = dr + ((sr - dr) * a >> 8), g = dg + ((sg - dg) * a >> 8), b = db + ((sb - db) * a >> 8);
    *p = (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

static void fill_ellipse(ctx_t *c, float cx, float cy, float rx, float ry,
                         uint32_t rgb, int alpha) {
    int y0 = (int)(cy - ry), y1 = (int)(cy + ry);
    for (int y = y0; y <= y1; y++) {
        float t = (y - cy) / ry;
        float w = 1 - t * t;
        if (w <= 0) continue;
        float half = rx * sqrtf(w);
        int x0 = (int)(cx - half), x1 = (int)(cx + half);
        for (int x = x0; x <= x1; x++)
            if (alpha >= 255) px(c, x, y, rgb565(rgb, c->dim));
            else px_blend(c, x, y, rgb, alpha);
    }
}

/* filled convex polygon, points in world space */
static void fill_poly(ctx_t *c, const float *xs, const float *ys, int n, uint32_t rgb) {
    float miny = 1e9f, maxy = -1e9f;
    for (int i = 0; i < n; i++) { if (ys[i] < miny) miny = ys[i]; if (ys[i] > maxy) maxy = ys[i]; }
    uint16_t col = rgb565(rgb, c->dim);
    for (int y = (int)miny; y <= (int)maxy; y++) {
        float x0 = 1e9f, x1 = -1e9f;
        for (int i = 0; i < n; i++) {
            int j = (i + 1) % n;
            float ay = ys[i], by = ys[j];
            if ((ay <= y && by > y) || (by <= y && ay > y)) {
                float x = xs[i] + (xs[j] - xs[i]) * (y - ay) / (by - ay);
                if (x < x0) x0 = x;
                if (x > x1) x1 = x;
            }
        }
        for (int x = (int)x0; x <= (int)x1; x++) px(c, x, y, col);
    }
}

static uint32_t mix(uint32_t a, uint32_t b, float p) {
    int ar = a >> 16 & 255, ag = a >> 8 & 255, ab = a & 255;
    int br = b >> 16 & 255, bg = b >> 8 & 255, bb = b & 255;
    return ((uint32_t)(ar + (br - ar) * p) << 16) |
           ((uint32_t)(ag + (bg - ag) * p) << 8) |
            (uint32_t)(ab + (bb - ab) * p);
}

static int popcount32(uint32_t v) { int n = 0; while (v) { n += v & 1; v >>= 1; } return n; }

/* per-fish roll state: +1 = normal, -1 = mirrored (facing left). Fish roll to
 * keep their back up instead of swimming inverted; animating the transition
 * reads as the fish turning over. Render-local so tank state stays pure. */
static float g_roll[N_FISH_MAX] = { 1, 1, 1, 1, 1, 1 };

/* The fish's body is a ledger of its life: stage sets size (tank.c) and fin
 * elaboration, hunger drains saturation, and earned markings stay:
 *   shadow survived -> an extra stripe; shrugged a shadow off -> a brow mark;
 *   elder -> a longer tail and a dorsal crest. All procedural, zero flash. */
static void draw_fish(ctx_t *c, const tank_t *t, const fish_t *f, int idx) {
    float tail = sinf(t->clock * (8 + f->speed * 0.055f) + f->wander) *
                 (0.32f + f->speed * 0.007f);
    float stress = f->stress / 10.0f;
    float pale = f->hunger > 7 ? (f->hunger - 7) / 3.0f * 0.35f : 0;   /* hungry = washed out */
    float ch = cosf(f->heading), sh = sinf(f->heading);
    float want = ch < -0.05f ? -1.0f : ch > 0.05f ? 1.0f : g_roll[idx];
    g_roll[idx] += (want - g_roll[idx]) * 0.18f;
    float roll = g_roll[idx];
    bool elder = f->stage == STAGE_ELDER, grown = f->stage >= STAGE_ADULT;
    float tail_len = elder ? 1.22f : 1.0f;
    uint32_t body = mix(mix(f->color, 0xf25b65, stress * 0.22f), 0x7a8a8e, pale);
    uint32_t fin  = mix(mix(f->fin, 0xf25b65, stress * 0.18f), 0x5a6a6e, pale);
#define TX(lx, ly) (f->x + ((lx) * ch - (ly) * roll * sh) * f->size)
#define TY(lx, ly) (f->y + ((lx) * sh + (ly) * roll * ch) * f->size)

    /* tail fin (triangle, flaps with tail phase) */
    {
        float lx[3] = {-11, -26 * tail_len, -26 * tail_len}, ly[3] = {0, -10 - tail * 8, 10 + tail * 8};
        float xs[3], ys[3];
        for (int i = 0; i < 3; i++) { xs[i] = TX(lx[i], ly[i]); ys[i] = TY(lx[i], ly[i]); }
        fill_poly(c, xs, ys, 3, fin);
    }
    /* dorsal crest: adults a small fin, elders a taller one */
    if (grown) {
        float h = elder ? -17 : -13;
        float lx[3] = {6, -2, -9}, ly[3] = {-8, h, -8};
        float xs[3], ys[3];
        for (int i = 0; i < 3; i++) { xs[i] = TX(lx[i], ly[i]); ys[i] = TY(lx[i], ly[i]); }
        fill_poly(c, xs, ys, 3, fin);
    }
    /* body: sampled outline of the prototype's quadratic silhouette */
    {
        static const float blx[10] = { 16, 10, 2, -8, -15, -18, -15, -8, 2, 10 };
        static const float bly[10] = { 0, -7, -10, -9, -6, 0, 6, 9, 10, 7 };
        float xs[10], ys[10];
        for (int i = 0; i < 10; i++) { xs[i] = TX(blx[i], bly[i]); ys[i] = TY(blx[i], bly[i]); }
        fill_poly(c, xs, ys, 10, body);
    }
    /* accent stripes (height tracks the roll so they flatten with the body);
       a fourth stripe is the mark of a shadow survived */
    float rmag = roll < 0 ? -roll : roll;
    int first = (f->ms_bits & MS_FIRST_SHADOW_SURVIVED) ? -2 : -1;
    for (int i = first; i <= 1; i++)
        fill_ellipse(c, TX(-3 + i * 6, 0), TY(-3 + i * 6, 0),
                     2, 1.5f + 3.5f * rmag, f->accent, 150);
    /* brow mark: shrugged off a shadow */
    if (f->ms_bits & MS_FIRST_SHRUG)
        fill_ellipse(c, TX(9, -6.5f), TY(9, -6.5f), 2.4f, 1.0f, f->accent, 200);
    /* eye — closed to a lid line when asleep (resting at night) */
    if (t->night && f->goal.id == GOAL_REST) {
        fill_ellipse(c, TX(9, -3), TY(9, -3), 2.2f, 0.7f, 0x9fb4b8, 200);
    } else {
        fill_ellipse(c, TX(9, -3), TY(9, -3), 2.2f, 2.2f, 0xffffff, 255);
        fill_ellipse(c, TX(9.7f, -3), TY(9.7f, -3), 1.1f, 1.1f, 0x031015, 255);
    }
#undef TX
#undef TY
}

/* a bed of swaying seaweed fronds; seed varies phase/heights between beds */
static void draw_veg(ctx_t *c, const tank_t *t, float bx0, int n, int max_seg, int seed) {
    for (int i = 0; i < n; i++) {
        float bx = bx0 + i * 12;
        uint32_t h = (uint32_t)((i + seed) * 2654435761u);
        int segs = max_seg - (int)(h % 5);
        float sway = sinf(t->clock * 0.9f + (i + seed) * 1.7f) * 4;
        for (int seg = 0; seg < segs; seg++) {
            float yy = TANK_H - 16 - seg * 3.2f;
            float xx = bx + sway * seg / (float)segs *
                       sinf(seg * 0.4f + t->clock * 0.6f + i + seed);
            fill_ellipse(c, xx, yy, 2.4f - seg * 0.07f, 2.2f,
                         (i + seed) & 1 ? 0x2e7d4f : 0x3f8b55, 220);
        }
    }
}

/* optional per-stage frame profiling (render.h) */
int64_t (*render_clock_us)(void) = NULL;
int64_t render_prof_us[6];
#define PROF_MARK() (render_clock_us ? render_clock_us() : 0)
#define PROF_ADD(i, t0) do { if (render_clock_us) { int64_t _n = render_clock_us(); render_prof_us[i] += _n - (t0); (t0) = _n; } } while (0)

/* ---- static scene: gradient, pebbles, reef rock (cacheable) ---- */
static uint16_t *g_scene = NULL;
static float     g_scene_dim = -1;
static unsigned  g_scene_epoch = 0;
static const uint16_t *g_primed_fb = NULL;
static unsigned  g_primed_epoch = 0;
static uint8_t  *g_vig = NULL;                  /* per-pixel vignette alpha (static) */
static bool      g_vig_filled = false;
void render_set_scene_cache(uint16_t *buf) { g_scene = buf; g_scene_dim = -1; }
void render_set_vignette_cache(uint8_t *buf) { g_vig = buf; g_vig_filled = false; }

const uint16_t *render_scene_buf(unsigned *epoch) {
    if (epoch) *epoch = g_scene_epoch;
    return g_scene_epoch ? g_scene : NULL;
}
void render_fb_primed(const uint16_t *fb, unsigned epoch) { g_primed_fb = fb; g_primed_epoch = epoch; }

/* vignette alpha at (x,y); matches the classic per-pixel loop */
static inline int vig_alpha(int x, int y) {
    float dx = (x - TANK_W * 0.5f) / (TANK_W * 0.5f);
    float dy = (y - TANK_H * 0.5f) / (TANK_H * 0.5f);
    float d2 = dx * dx + dy * dy;
    if (d2 <= 0.72f) return 0;
    int a = (int)((d2 - 0.72f) * 220);
    return a > 255 ? 255 : a;
}

/* pure darkening (blend toward black), independent of ctx dim */
static inline void px_darken(uint16_t *p, int a) {
    int inv = 256 - a;
    *p = (uint16_t)(((((*p >> 11) * inv) >> 8) << 11) |
                    ((((((*p) >> 5) & 63) * inv) >> 8) << 5) |
                    (((*p & 31) * inv) >> 8));
}


static void draw_scene(const tank_t *t, uint16_t *fb, int stride, float dim) {
    ctx_t c = { fb, stride, dim };
    /* water gradient #0a3c46 → #08272f → #031015 */
    for (int y = 0; y < TANK_H; y++) {
        float p = (float)y / TANK_H;
        uint32_t col = p < 0.45f ? mix(0x0a3c46, 0x08272f, p / 0.45f)
                                 : mix(0x08272f, 0x031015, (p - 0.45f) / 0.55f);
        uint16_t v = rgb565(col, dim);
        for (int x = 0; x < TANK_W; x++) fb[y * stride + x] = v;
    }
    /* pebbled bottom: irregular top edge, speckled stones in three tones.
       All variation comes from a position hash so it is stable every frame. */
    for (int x = 0; x < TANK_W; x++) {
        uint32_t h = (uint32_t)(x * 2654435761u);
        int top = TANK_H - 14 - (int)((h >> 8) % 5);
        for (int y = top; y < TANK_H; y++) {
            uint32_t h2 = (uint32_t)((x * 73856093u) ^ (y * 19349663u));
            uint32_t tone = (h2 >> 4) % 16;
            uint32_t col = tone < 2 ? 0x2e3b2c : tone < 5 ? 0x22301f
                         : tone < 8 ? 0x1a2418 : 0x101a12;
            fb[y * stride + x] = rgb565(col, dim);
        }
    }
    /* reef rock (the entity fish know); it widens as the tank earns milestones */
    float grow = 1.0f + 0.06f * popcount32(t->tank_ms_bits);
    fill_ellipse(&c, t->reef_x, TANK_H - 16, 34 * grow, 10 + 2 * (grow - 1) * 10, 0x123028, 255);
}

void render_tank(const tank_t *t, uint16_t *fb, int stride) {
    float dim = t->night ? 0.45f : 1.0f;
    ctx_t c = { fb, stride, dim };
    int64_t p0 = PROF_MARK();
    bool cached = g_scene && stride == TANK_W;
    struct { short x0, y0, x1, y1; } rects[4 + MAX_FOOD + MAX_BUBBLE + N_FISH_MAX];
    int nr = 0;
#define DYN_RECT(cx0, cy0, cx1, cy1) do { if (cached && nr < (int)(sizeof rects / sizeof rects[0])) { \
        rects[nr].x0 = (short)(cx0); rects[nr].y0 = (short)(cy0); \
        rects[nr].x1 = (short)(cx1); rects[nr].y1 = (short)(cy1); nr++; } } while (0)

    if (cached) {
        if (g_scene_dim != dim) {
            /* rebuild the static scene and bake the vignette into it (the
               per-frame pass then only re-darkens dynamic patches) */
            draw_scene(t, g_scene, TANK_W, dim);
            ctx_t sc = { g_scene, TANK_W, dim };
            for (int y = 0; y < TANK_H; y++)
                for (int x = 0; x < TANK_W; x++) {
                    int a = (g_vig && g_vig_filled) ? g_vig[y * TANK_W + x] : vig_alpha(x, y);
                    if (g_vig && !g_vig_filled) g_vig[y * TANK_W + x] = (uint8_t)a;
                    if (a) px_darken(&sc.fb[y * TANK_W + x], a);
                }
            if (g_vig) g_vig_filled = true;
            g_scene_dim = dim; g_scene_epoch++;
        }
        if (!(g_primed_fb == fb && g_primed_epoch == g_scene_epoch))
            memcpy(fb, g_scene, TANK_W * TANK_H * sizeof(uint16_t));
        g_primed_fb = NULL;
    } else draw_scene(t, fb, stride, dim);
    PROF_ADD(0, p0);

    /* light shafts (subtle, day only) */
    if (!t->night)
        for (int i = 0; i < 3; i++) {
            float sx = 60 + i * 150 + sinf(t->clock * 0.3f + i) * 18;
            for (int y = 0; y < TANK_H * 2 / 3; y++)
                for (int x = -8; x <= 8; x++)
                    px_blend(&c, (int)(sx + x + y * 0.22f), y, 0x2a6a72, 14 - (x < 0 ? -x : x));
        }
    PROF_ADD(1, p0);
    /* vegetation: the reef proper (left - grows lusher as the tank earns
       milestones, never withers) and balancing decorative beds on the right
       (scenery only, not in the schema) */
    int lush = popcount32(t->tank_ms_bits); if (lush > 4) lush = 4;
    float vx0 = t->reef_x - 24 - lush * 6; int vn0 = 5 + lush, vs0 = 14 + lush;
    draw_veg(&c, t, vx0, vn0, vs0, 0);
    draw_veg(&c, t, TANK_W * 0.84f, 4, 18, 7);
    draw_veg(&c, t, TANK_W * 0.62f, 2, 8, 3);
    DYN_RECT((int)vx0 - 8, TANK_H - 16 - (int)(vs0 * 3.2f) - 4, (int)vx0 + vn0 * 12 + 8, TANK_H - 1);
    DYN_RECT((int)(TANK_W * 0.84f) - 8, TANK_H - 16 - (int)(18 * 3.2f) - 4, (int)(TANK_W * 0.84f) + 4 * 12 + 8, TANK_H - 1);
    DYN_RECT((int)(TANK_W * 0.62f) - 8, TANK_H - 16 - (int)(8 * 3.2f) - 4, (int)(TANK_W * 0.62f) + 2 * 12 + 8, TANK_H - 1);
    PROF_ADD(2, p0);
    /* food pellets */
    for (int i = 0; i < MAX_FOOD; i++)
        if (t->food[i].alive) {
            fill_ellipse(&c, t->food[i].x, t->food[i].y, 2.6f, 2.6f, 0xffbd59, 255);
            fill_ellipse(&c, t->food[i].x - 0.8f, t->food[i].y - 0.8f, 1.0f, 1.0f, 0xffe9bd, 255);
            DYN_RECT((int)t->food[i].x - 5, (int)t->food[i].y - 5, (int)t->food[i].x + 5, (int)t->food[i].y + 5);
        }
    /* bubbles */
    for (int i = 0; i < MAX_BUBBLE; i++) {
        const bubble_t *b = &t->bubble[i];
        float r = b->column ? 2.6f : 1.8f;
        fill_ellipse(&c, b->x, b->y, r, r, 0x9fd8e2, 60);
        px_blend(&c, (int)(b->x - r * 0.4f), (int)(b->y - r * 0.4f), 0xffffff, 120);
        DYN_RECT((int)b->x - 5, (int)b->y - 5, (int)b->x + 5, (int)b->y + 5);
    }
    PROF_ADD(3, p0);
    /* fish */
    for (int i = 0; i < t->n_fish; i++) {
        draw_fish(&c, t, &t->fish[i], i);
        int h = (int)(40 * t->fish[i].size) + 4;
        DYN_RECT((int)t->fish[i].x - h, (int)t->fish[i].y - h, (int)t->fish[i].x + h, (int)t->fish[i].y + h);
    }
    /* shadow overlay */
    if (t->shadow.active) {
        float fade = t->shadow.ttl < 2.2f ? t->shadow.ttl / 2.2f : 1.0f;
        fill_ellipse(&c, t->shadow.x, t->shadow.y,
                     t->shadow.size, t->shadow.size * 0.34f, 0x00070a,
                     (int)(120 * fade));
        DYN_RECT((int)(t->shadow.x - t->shadow.size) - 2, (int)(t->shadow.y - t->shadow.size * 0.34f) - 2,
                 (int)(t->shadow.x + t->shadow.size) + 2, (int)(t->shadow.y + t->shadow.size * 0.34f) + 2);
    }
    PROF_ADD(4, p0);
    /* porthole vignette: darken corners toward AMOLED black. With a scene
       cache the full-frame pass is baked into the scene and only the dynamic
       patches are re-darkened; without one, the classic per-pixel pass runs. */
    if (cached) {
        /* one row sweep over the union of the dynamic rects: pixels that
           differ from the baked scene were drawn this frame and get the
           vignette re-applied exactly once. */
        for (int y = 0; y < TANK_H; y++) {
            float dyf = (y - TANK_H * 0.5f) / (TANK_H * 0.5f);
            float rem = 0.72f - dyf * dyf;
            int x_in = rem > 0 ? (int)(TANK_W * 0.5f * (1 - sqrtf(rem))) : TANK_W / 2;
            if (x_in <= 0) continue;
            short iv[sizeof rects / sizeof rects[0]][2]; int ni = 0;
            for (int i = 0; i < nr; i++)
                if (y >= rects[i].y0 && y <= rects[i].y1) {
                    int a0 = rects[i].x0 < 0 ? 0 : rects[i].x0;
                    int a1 = rects[i].x1 >= TANK_W ? TANK_W - 1 : rects[i].x1;
                    if (a0 > a1) continue;
                    int j = ni++;                       /* insertion sort by x0 */
                    while (j > 0 && iv[j - 1][0] > a0) { iv[j][0] = iv[j - 1][0]; iv[j][1] = iv[j - 1][1]; j--; }
                    iv[j][0] = (short)a0; iv[j][1] = (short)a1;
                }
            int end = -1;                               /* merged sweep */
            for (int i = 0; i < ni; i++) {
                int a0 = iv[i][0] > end + 1 ? iv[i][0] : end + 1;
                int a1 = iv[i][1];
                if (a1 > end) end = a1;
                for (int s = 0; s < 2; s++) {           /* clip to the two ring spans */
                    int r0 = s ? (TANK_W - x_in > a0 ? TANK_W - x_in : a0) : a0;
                    int r1 = s ? a1 : (x_in - 1 < a1 ? x_in - 1 : a1);
                    for (int x = r0; x <= r1; x++) {
                        uint16_t *p = &c.fb[y * c.stride + x];
                        if (*p == g_scene[y * TANK_W + x]) continue;
                        int a = (g_vig && g_vig_filled) ? g_vig[y * TANK_W + x] : vig_alpha(x, y);
                        if (a) px_darken(p, a);
                    }
                }
            }
        }
    } else {
        for (int y = 0; y < TANK_H; y++) {
            float dy = (y - TANK_H * 0.5f) / (TANK_H * 0.5f);
            float rem = 0.72f - dy * dy;
            int x_in = rem > 0 ? (int)(TANK_W * 0.5f * (1 - sqrtf(rem))) : TANK_W / 2;
            for (int side = 0; side < 2; side++)
                for (int k = 0; k < x_in; k++) {
                    int x = side ? TANK_W - 1 - k : k;
                    float dx = (x - TANK_W * 0.5f) / (TANK_W * 0.5f);
                    float d2 = dx * dx + dy * dy;
                    if (d2 > 0.72f) {
                        int a = (int)((d2 - 0.72f) * 220);
                        if (a > 0) px_blend(&c, x, y, 0x000000, a > 255 ? 255 : a);
                    }
                }
        }
    }
    PROF_ADD(5, p0);
#undef DYN_RECT
}

/* device battery pill, top-right: outline + nub, fill fraction colored by
 * level (charging = teal). Same visual language as the stats card - no text. */
void render_battery(uint16_t *fb, int stride, float frac, bool charging) {
    ctx_t c = { fb, stride, 1.0f };
    if (frac < 0) frac = 0;
    if (frac > 1) frac = 1;
    const int W = 26, H = 11, X = TANK_W - W - 28, Y = 9;   /* clear of the curved bezel */
    uint32_t col = charging ? 0x38dcc7 : frac < 0.2f ? 0xf25b65
                 : frac < 0.45f ? 0xffbd59 : 0x78d67d;
    for (int y = Y; y < Y + H; y++)
        for (int x = X; x < X + W; x++)
            px_blend(&c, x, y, 0x04141a, 215);
    for (int x = X; x < X + W; x++) { px(&c, x, Y, rgb565(0x9fb4b8, 1)); px(&c, x, Y + H - 1, rgb565(0x9fb4b8, 1)); }
    for (int y = Y; y < Y + H; y++) { px(&c, X, y, rgb565(0x9fb4b8, 1)); px(&c, X + W - 1, y, rgb565(0x9fb4b8, 1)); }
    for (int y = Y + 3; y < Y + H - 3; y++)                    /* nub */
        for (int x = X + W; x < X + W + 3; x++) px(&c, x, y, rgb565(0x9fb4b8, 1));
    int fw = (int)((W - 4) * frac + 0.5f);
    for (int y = Y + 2; y < Y + H - 2; y++)
        for (int x = X + 2; x < X + 2 + fw; x++) px(&c, x, y, rgb565(col, 1));
}

/* ---- stats overlay (selection ring + visual card) ---- */

static void ring(ctx_t *c, float cx, float cy, float r, uint32_t rgb) {
    for (int i = 0; i < 64; i++) {
        float a = i * (TAU / 64);
        px_blend(c, (int)(cx + cosf(a) * r), (int)(cy + sinf(a) * r), rgb, 180);
    }
}

static void bar(ctx_t *c, int x, int y, float frac, uint32_t rgb, bool revealed) {
    fill_ellipse(c, x + 2, y + 2, 2.4f, 2.4f, rgb, revealed ? 255 : 90);   /* legend dot */
    for (int yy = 0; yy < 5; yy++)                             /* track */
        for (int xx = 0; xx < 70; xx++)
            if (revealed || (xx & 4)) px_blend(c, x + 8 + xx, y + yy, 0x2a3f45, revealed ? 160 : 90);
    if (!revealed) return;                                     /* not yet discovered */
    int w = (int)(70 * (frac < 0 ? 0 : frac > 1 ? 1 : frac));
    for (int yy = 0; yy < 5; yy++)                             /* fill */
        for (int xx = 0; xx < w; xx++)
            px_blend(c, x + 8 + xx, y + yy, rgb, 235);
}

void render_stats_card(const tank_t *t, int fish_idx, uint16_t *fb, int stride) {
    if (fish_idx < 0 || fish_idx >= t->n_fish) return;
    ctx_t c = { fb, stride, 1.0f };            /* card ignores night dimming */
    const fish_t *f = &t->fish[fish_idx];

    ring(&c, f->x, f->y, 17 * f->size, f->color);

    /* card: top-left, bordered in the fish's own color (that's its "name") */
    const int X = 14, Y = 8, W = 92, H = 84;   /* x clear of the curved bezel */
    for (int y = Y; y < Y + H; y++)
        for (int x = X; x < X + W; x++)
            px_blend(&c, x, y, 0x04141a, 215);
    for (int x = X; x < X + W; x++) { px(&c, x, Y, rgb565(f->color, 1)); px(&c, x, Y + H - 1, rgb565(f->color, 1)); }
    for (int y = Y; y < Y + H; y++) { px(&c, X, y, rgb565(f->color, 1)); px(&c, X + W - 1, y, rgb565(f->color, 1)); }

    /* drives (0..10) then personality (0..1); personality is revealed by
       behaviour you have seen this fish do */
    bool saw_bold = f->ms_bits & (MS_FIRST_DART | MS_FIRST_SHRUG);
    bool saw_social = f->ms_bits & MS_FIRST_FOLLOW;
    bool saw_curious = f->ms_bits & (MS_FIRST_REEF | MS_FIRST_BUBBLES);
    bar(&c, X + 5, Y + 6,  f->hunger / 10.0f,    0xffbd59, true);        /* hunger: pellet-amber */
    bar(&c, X + 5, Y + 15, f->energy / 10.0f,    0x78d67d, true);        /* energy: green */
    bar(&c, X + 5, Y + 24, f->stress / 10.0f,    0xf25b65, true);        /* stress: red */
    bar(&c, X + 5, Y + 33, f->curiosity / 10.0f, 0x6db9ff, saw_curious); /* curiosity: blue */
    bar(&c, X + 5, Y + 44, f->bold,              0xffffff, saw_bold);    /* bold: white */
    bar(&c, X + 5, Y + 53, f->sociable,          0x38dcc7, saw_social);  /* social: teal */
    bar(&c, X + 5, Y + 62, f->trust / 10.0f,     0xffd166, true);        /* trust: gold */

    /* stage pips: fry=1 .. elder=4 */
    for (int i = 0; i <= (int)f->stage; i++)
        fill_ellipse(&c, X + 10 + i * 9, Y + 76, 2.6f, 2.6f, f->accent, 255);
    /* on its mind: a faint dot for how sure the last decision was (bright =
       certain, dim = torn) - the distribution layer made visible */
    fill_ellipse(&c, X + W - 12, Y + 76, 3.0f, 3.0f, 0xffffff, (int)(40 + 200 * f->goal.confidence));
}

/* ---- milestones view ---- */
void render_milestones(const tank_t *t, uint16_t *fb, int stride) {
    ctx_t c = { fb, stride, 1.0f };
    for (int y = 0; y < TANK_H; y++)
        for (int x = 0; x < TANK_W; x++) fb[y * stride + x] = rgb565(0x031015, 1);
    const int X0 = 40, PIP = 26, ROW = 40, Y0 = 36;
    /* per-fish rows */
    for (int i = 0; i < t->n_fish; i++) {
        const fish_t *f = &t->fish[i];
        int y = Y0 + i * ROW;
        fill_ellipse(&c, X0 - 18, y, 7, 7, f->color, 255);               /* identity swatch */
        for (int s = 0; s <= (int)f->stage; s++)                          /* stage pips under it */
            fill_ellipse(&c, X0 - 27 + s * 6, y + 12, 1.8f, 1.8f, f->accent, 255);
        for (int m = 0; m < MS_FISH_COUNT; m++) {
            bool on = f->ms_bits & (1u << m);
            int x = X0 + 10 + m * PIP;
            if (on) { fill_ellipse(&c, x, y, 7, 7, f->color, 255); fill_ellipse(&c, x, y, 3, 3, f->accent, 255); }
            else ring(&c, x, y, 6, 0x2a3f45);
        }
    }
    /* tank row */
    int y = Y0 + N_FISH_MAX * ROW + 10;
    for (int x = X0 - 26; x < TANK_W - 30; x++) px_blend(&c, x, y - 20, 0x2a3f45, 200);
    fill_ellipse(&c, X0 - 18, y, 7, 4, 0x9fd8e2, 255);
    for (int m = 0; m < TMS_COUNT; m++) {
        bool on = t->tank_ms_bits & (1u << m);
        int x = X0 + 10 + m * PIP;
        if (on) { fill_ellipse(&c, x, y, 7, 7, 0x9fd8e2, 255); fill_ellipse(&c, x, y, 3, 3, 0x031015, 255); }
        else ring(&c, x, y, 6, 0x2a3f45);
    }
    /* empty slots still to arrive: faint rings where a fish row would be */
    for (int i = t->n_fish; i < N_FISH_MAX; i++) ring(&c, X0 - 18, Y0 + i * ROW, 7, 0x1a2a30);
}
