/* render.h — software renderer: draws the tank into a raw RGB565 framebuffer.
 * No LVGL/SDL dependency; the same code runs on the ESP32 (the display port
 * just decides where the buffer goes). */
#ifndef RENDER_H
#define RENDER_H

#include "tank.h"

/* fb is TANK_W x TANK_H, RGB565, stride in PIXELS (usually TANK_W). */
void render_tank(const tank_t *t, uint16_t *fb, int stride);

/* Optional frame profiling: set a microsecond clock and render_tank fills
 * render_prof_us per stage (0 scene-copy, 1 shafts, 2 veg, 3 food+bubbles,
 * 4 fish, 5 vignette). Accumulates until the caller zeroes it. NULL = off. */
extern int64_t (*render_clock_us)(void);
extern int64_t render_prof_us[6];

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
void render_fb_primed(const uint16_t *fb, unsigned epoch);

/* Stats overlay for one selected fish: selection ring + a small card of
 * visual bars (drives + personality) and stage pips. No text, no digits —
 * the progression design's "simple and visual" stats view. Personality bars
 * are revealed only after the fish has shown that side of itself (milestone
 * bits): you learn your fish by watching. */
void render_stats_card(const tank_t *t, int fish_idx, uint16_t *fb, int stride);

/* Device battery pill (top-right), drawn with the stats card on hardware:
 * frac 0..1, charging tints the fill teal. */
void render_battery(uint16_t *fb, int stride, float frac, bool charging);

/* Milestones view (separate screen, never on the tank): one row per fish in
 * its own color with a pip per first it has reached, and the tank's own row
 * at the bottom. Visual only. */
void render_milestones(const tank_t *t, uint16_t *fb, int stride);

#endif
