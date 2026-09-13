/* setup.h — first-run setup (2026-09-13): the welcome, a name and colours for
 * each of the two fry, then the care tips - for a fresh install and after a
 * reset. Platform-agnostic: the state machine, the gestures, the hit
 * geometry and the drawing all live here (the reset prompt kept its state in
 * each platform's touch code; a five-page flow would have been written
 * twice), built on render.h's UI primitives over the LIVE tank - the fish
 * keep swimming behind the pages and wear the colours as they are picked.
 *
 * The platforms do three things: setup_begin when progression says the flow
 * is pending (boot, a reset YES, the director's cue); setup_touch every
 * frame with the finger's position and state while it is up (taps and the
 * letter-wheel drags are classified here); and render_setup after
 * render_tank while setup_active. Every other gesture is swallowed while it
 * is up. No timeout: it waits for the keeper.
 *
 * Naming (third design, Strato: "seeing the fish while naming it is very
 * important" - and a keyboard's keys are too small for a 35 mm glass): NO
 * panel - the tank stays in view with a ring around the fish being named -
 * and seven big letter slots across the middle, arcade high-score style.
 * Touch a slot to pick it, then drag up / down on it (or tap the chevrons
 * above and below) to spin its letter through blank, A..Z. A blank at the
 * end shortens the name; NEXT trims blanks and puts the preset's name back
 * if nothing is left. Targets are a slot wide (44 px) and the whole band
 * tall, so nothing here needs aim. */
#ifndef POCKET_TANK_SETUP_H
#define POCKET_TANK_SETUP_H
#include "tank.h"
#include <stdint.h>
#include <stdbool.h>

void setup_begin(tank_t *t);             /* page 1 (needs 2 fish; fewer = done at once) */
bool setup_active(void);
void setup_cancel(tank_t *t);            /* drop the pages; the save still says pending */
int  setup_page(void);                   /* SETUP_PG_* while active */
/* the finger, every frame (or poll): x,y in tank coordinates, down = touching.
 * A press then release on one element taps it; a press on a letter slot and
 * a vertical drag spins that letter (SETUP_SPIN_PX of travel per step). */
void setup_touch(tank_t *t, float x, float y, bool down);
/* the pieces, for tests and the touch log: the element under (x,y) on the
 * current page (0 = none) and a tap on it */
int  setup_hit(float x, float y);
void setup_activate(tank_t *t, int id);
const char *setup_hit_name(int id);      /* "NEXT", "slot 2", "up", "body 3"... */
int  setup_slot(void);                   /* the active letter slot on a name page */
void render_setup(const tank_t *t, uint16_t *fb, int stride, float clock);

/* pages, in order */
enum { SETUP_PG_WELCOME, SETUP_PG_NAME_A, SETUP_PG_LOOK_A, SETUP_PG_NAME_B, SETUP_PG_LOOK_B, SETUP_PG_CARE, SETUP_PG_N };
/* element ids (setup_hit / setup_activate) */
#define SETUP_HIT_NEXT  1
#define SETUP_HIT_BACK  2
#define SETUP_HIT_UP    3                /* the active slot's letter: next (A -> B) */
#define SETUP_HIT_DOWN  4                /* ... previous */
#define SETUP_HIT_SLOT0 10               /* + 0..FISH_NAME_MAX-1: pick that slot */
#define SETUP_HIT_BODY0 40               /* + swatch 0..LOOK_N-1 */

/* geometry (tank coordinates), shared by the drawing, the hit test and the
 * sim's selftest. The panelled pages (welcome, colours, care) sit inside the
 * bezel curve (x 32..416, y 16..342); the name page draws straight on the
 * tank. */
#define SETUP_X 32
#define SETUP_Y 16
#define SETUP_W 384
#define SETUP_H 326
#define SETUP_BTN_W 110
#define SETUP_BTN_H 42
#define SETUP_BTN_Y (SETUP_Y + SETUP_H - 12 - SETUP_BTN_H)   /* welcome / care: the foot */
#define SETUP_BACK_X (SETUP_X + 16)
#define SETUP_NEXT_X (SETUP_X + SETUP_W - 16 - SETUP_BTN_W)
#define SETUP_MID_X  (SETUP_X + (SETUP_W - SETUP_BTN_W) / 2)
#define SETUP_TOP_BTN_W 84                                 /* name / look: the top row */
#define SETUP_TOP_BTN_Y (SETUP_Y + 24)
#define SETUP_TOP_BACK_X (SETUP_X + 12)
#define SETUP_TOP_NEXT_X (SETUP_X + SETUP_W - 12 - SETUP_TOP_BTN_W)
/* the letter wheel: FISH_NAME_MAX slots of a 6x font (30 x 42 px glyphs) at
 * a 44 px pitch across the middle of the tank, chevrons 40 px above and
 * below the active one; a press in the row band picks the nearest slot, the
 * bands above / below it are the chevrons' */
#define SETUP_SLOT_SCALE 6
#define SETUP_SLOT_PX  44
#define SETUP_SLOT_W   (5 * SETUP_SLOT_SCALE)
#define SETUP_SLOT_H   (7 * SETUP_SLOT_SCALE)
#define SETUP_SLOT_X   ((TANK_W - ((FISH_NAME_MAX - 1) * SETUP_SLOT_PX + SETUP_SLOT_W)) / 2)
#define SETUP_SLOT_Y   150
#define SETUP_ARROW_GAP 40
#define SETUP_SPIN_PX   30                                 /* drag travel per letter */
/* the colour page (third design too): no panel, the live FRY wears the pick
 * with a ring round it; one row of 8 body swatches, 42 x 60 px at a 46 px
 * pitch, a tall band around it so a low-landing finger still hits; the
 * accent is a "?" - a fry's markings come in as it grows */
#define SETUP_SW_N  LOOK_N
#define SETUP_SW_PX 46
#define SETUP_SW_W  42
#define SETUP_SW_H  60
#define SETUP_SW_X  ((TANK_W - (SETUP_SW_N - 1) * SETUP_SW_PX - SETUP_SW_W) / 2)
#define SETUP_SW_Y  176
#define SETUP_ACC_Y 276
/* the stage: the clear spot each page leaves for the fish being edited
 * (tank_t.stage_*), top centre between the buttons */
#define SETUP_STAGE_X   (TANK_W / 2)
#define SETUP_STAGE_NAME_Y 94
#define SETUP_STAGE_LOOK_Y 112
#endif
