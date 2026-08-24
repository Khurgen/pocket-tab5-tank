/* render.h — software renderer: draws the tank into a raw RGB565 framebuffer.
 * No LVGL/SDL dependency; the same code runs on the ESP32 (the display port
 * just decides where the buffer goes). */
#ifndef RENDER_H
#define RENDER_H

#include "tank.h"

/* fb is TANK_W x TANK_H, RGB565, stride in PIXELS (usually TANK_W). */
void render_tank(const tank_t *t, uint16_t *fb, int stride);

/* Optional static-scene cache (TANK_W*TANK_H uint16): the water gradient,
 * pebbles and reef rock are rendered once per lighting state and copied each
 * frame instead of recomputed - keeps core 0's frame budget flat as the scene
 * gets lusher. NULL disables. */
void render_set_scene_cache(uint16_t *buf);

/* Stats overlay for one selected fish: selection ring + a small card of
 * visual bars (drives + personality) and stage pips. No text, no digits —
 * the progression design's "simple and visual" stats view. Personality bars
 * are revealed only after the fish has shown that side of itself (milestone
 * bits): you learn your fish by watching. */
void render_stats_card(const tank_t *t, int fish_idx, uint16_t *fb, int stride);

/* Milestones view (separate screen, never on the tank): one row per fish in
 * its own color with a pip per first it has reached, and the tank's own row
 * at the bottom. Visual only. */
void render_milestones(const tank_t *t, uint16_t *fb, int stride);

#endif
