/* render.h — software renderer: draws the tank into a raw RGB565 framebuffer.
 * No LVGL/SDL dependency; the same code runs on the ESP32 (the display port
 * just decides where the buffer goes). */
#ifndef RENDER_H
#define RENDER_H

#include "tank.h"

/* fb is TANK_W x TANK_H, RGB565, stride in PIXELS (usually TANK_W). */
void render_tank(const tank_t *t, uint16_t *fb, int stride);

/* Optional frame profiling: set a microsecond clock and render_tank fills
 * render_prof_us per stage (0 scene-copy, 1 shafts, 2 veg (back), 3 food+
 * bubbles, 4 fish + front veg, 5 vignette sweep, 6 algae film). Accumulates
 * until the caller zeroes it. NULL = off. */
extern int64_t (*render_clock_us)(void);
extern int64_t render_prof_us[7];

/* Optional static-scene cache (TANK_W*TANK_H uint16): the water gradient,
 * pebbles and reef rock are rendered once per lighting state and copied each
 * frame instead of recomputed - keeps core 0's frame budget flat as the scene
 * gets lusher. NULL disables. */
void render_set_scene_cache(uint16_t *buf);

/* Optional vignette alpha cache (TANK_W*TANK_H bytes): with a scene cache the
 * porthole vignette is baked into the scene and re-applied per frame only on
 * dynamic pixels; this LUT removes the per-pixel float math. NULL = compute. */
void render_set_vignette_cache(uint8_t *buf);

/* Scene prefetch support: render_scene_buf returns the built scene cache (or
 * NULL) and its epoch (bumped on lighting rebuilds). A platform that copies
 * the scene into fb ahead of time (e.g. by DMA) calls render_fb_primed; the
 * next render_tank into that fb at that epoch skips its own scene restore. */
const uint16_t *render_scene_buf(unsigned *epoch);

/* Dirty mask (RENDER_DIRTY_WORDS uint32): required with the scene cache -
 * marks the pixels drawn each frame so the vignette re-apply reads the mask,
 * not the scene. Without it the renderer falls back to full redraws. */
#define RENDER_DIRTY_WORDS (TANK_H * (TANK_W / 32))
void render_set_dirty_mask(uint32_t *buf);
void render_fb_primed(const uint16_t *fb, unsigned epoch);

/* Stats overlay for one selected fish: selection ring + a small card of
 * visual bars (drives + personality) and stage pips. No text, no digits —
 * the progression design's "simple and visual" stats view. Personality bars
 * are revealed only after the fish has shown that side of itself (milestone
 * bits): you learn your fish by watching. */
void render_stats_card(const tank_t *t, int fish_idx, uint16_t *fb, int stride);
/* Optional card cache (RENDER_CARD_W x RENDER_CARD_H uint16): with the scene
 * cache live, the card is redrawn at most 4x/s and blitted otherwise (~7 ms
 * -> ~1 ms per frame on the device). NULL = draw every frame. */
#define RENDER_CARD_X 14
#define RENDER_CARD_Y 8
#define RENDER_CARD_W 124
#define RENDER_CARD_H 228
void render_set_card_cache(uint16_t *buf);

/* Device battery pill (top-right), drawn with the stats card on hardware:
 * frac 0..1, charging tints the fill teal. */
void render_battery(uint16_t *fb, int stride, float frac, bool charging);

/* Milestones page (separate screen, never on the tank; 2026-09-13 redesign
 * on Strato's pixel-art badges): one row per fish - its sprite at its real
 * size, its name, a growth strip fry -> elder - and six event badges; the
 * tank's row below with the population strip and six tank badges. A locked
 * badge is the same picture as a grey silhouette; one earned since the
 * keeper last closed the page wears a ring. render_milestones_tap maps a
 * tap: a badge, a name or a strip opens a small detail modal (the art at
 * 2x, a title, the words); while the modal is up ANY tap closes it. A
 * CLOSE button at the bottom right leaves the page. Callers try it BEFORE
 * the brightness row. */
void render_milestones(const tank_t *t, uint16_t *fb, int stride);
/* a tap on the page (2026-09-13, Strato: with this much to tap, a stray tap
 * must not drop the whole page): MS_TAP_CLOSE = the CLOSE button, bottom
 * right - the ONLY way out by touch (caller closes the page, then
 * progression_ack_milestones + render_milestones_leave); MS_TAP_KEPT = a
 * badge / name / strip opened the detail modal, or the modal was up and
 * this tap closed it; MS_TAP_NONE = nothing here (the caller may try the
 * brightness row). */
enum { MS_TAP_NONE = 0, MS_TAP_KEPT = 1, MS_TAP_CLOSE = 2 };
int  render_milestones_tap(const tank_t *t, float x, float y);
void render_milestones_leave(void);

/* Reset confirm (2026-09-11): a modal panel over the live tank - "RESET
 * TANK?", what it costs, a NO and a YES button, and a bar draining toward
 * the timeout (frac 1 -> 0). The first text the renderer draws (a 5x7
 * pixel font, upper case). Drawn last, over the card / milestones page.
 * render_confirm_hit maps a tap in tank coordinates to a button (+1 YES,
 * -1 NO, 0 neither) so the device's touch port and the sim's mouse share
 * the geometry. */
#define RENDER_CONFIRM_X     56
#define RENDER_CONFIRM_Y     76
#define RENDER_CONFIRM_W     336
#define RENDER_CONFIRM_H     216
#define RENDER_CONFIRM_BTN_W 132
#define RENDER_CONFIRM_BTN_H 56
#define RENDER_CONFIRM_BTN_Y (RENDER_CONFIRM_Y + 112)
#define RENDER_CONFIRM_NO_X  (RENDER_CONFIRM_X + 24)
#define RENDER_CONFIRM_YES_X (RENDER_CONFIRM_X + RENDER_CONFIRM_W - 24 - RENDER_CONFIRM_BTN_W)
void render_confirm_reset(uint16_t *fb, int stride, float frac);
int  render_confirm_hit(float x, float y);

/* Brightness row (2026-09-11) at the foot of the milestones page: a caption,
 * three rising bars and the percentage. The device's panel level (100 / 60 /
 * 30 %) - a tap on the row cycles it instead of closing the page
 * (render_brightness_row_hit). The sim draws it too, for parity. */
void render_brightness_row(uint16_t *fb, int stride, int pct);
bool render_brightness_row_hit(float x, float y);

/* UI primitives (2026-09-13) for panels built outside this file (the first-
 * run setup in common/setup.c): the confirm prompt's pixel font, flat rects
 * and buttons, and a fish drawn on its own for a preview. All ignore the
 * night dim, like the card, and draw AFTER render_tank (nothing re-vignettes
 * them). Text is upper case + digits + a little punctuation; `scale` is the
 * pixel size of one font dot (2 = caption, 3 = button). */
int  render_text_w(const char *s, int scale);
void render_text(uint16_t *fb, int stride, int x, int y, int scale, uint32_t rgb, const char *s);
void render_rect(uint16_t *fb, int stride, int x, int y, int w, int h, uint32_t rgb);
void render_rect_blend(uint16_t *fb, int stride, int x, int y, int w, int h, uint32_t rgb, int alpha);   /* alpha 0..255 */
void render_rect_edge(uint16_t *fb, int stride, int x, int y, int w, int h, uint32_t rgb);
void render_ring(uint16_t *fb, int stride, float cx, float cy, float r, uint32_t rgb);   /* the card's selection ring */
void render_button(uint16_t *fb, int stride, int x, int y, int w, int h, uint32_t fill, uint32_t edge, const char *label, int scale);
/* an adult fish facing right at (x,y), body length ~42 x size px, tail
 * swimming on `clock` - the setup's live preview of a colour choice */
void render_fish_preview(uint16_t *fb, int stride, float x, float y, float size,
                         uint32_t body, uint32_t fin, uint32_t accent, float clock);
/* the same, but AS THE FISH IS: its own stage (a fry shows no markings yet,
 * an elder its long tail), calm and fed - the birth flow's portrait */
void render_fish_portrait(uint16_t *fb, int stride, float x, float y, float size, const fish_t *who, float clock);

#endif
