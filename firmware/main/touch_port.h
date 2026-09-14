#ifndef TOUCH_PORT_H
#define TOUCH_PORT_H
#include <stdbool.h>
#include <stdint.h>
#include "tank.h"
bool touch_port_init(void);
void touch_port_poll(tank_t *t);
int  touch_port_selected(void);   /* tapped fish for the stats card, -1 = none */
/* milestones page: a tap ON the open stats card flips to it; it has no
 * auto-dismiss - the next tap anywhere closes it (and the card, if still up).
 * touch_port_show_milestones is the director's cue (needs no card). */
bool touch_port_milestones(void);
void touch_port_show_milestones(bool on);
void touch_port_dismiss(void);    /* drop the card and the page: a flow (the birth flow) took the glass */
void touch_port_set_inverted(bool inverted);   /* mirror coords when the screen is flipped */
/* reset confirm prompt (render_confirm_reset): opened by main.c's chord -
 * BOOT held, then a finger lands on the glass - or the director's `reset`.
 * While it is up every other gesture is swallowed; a press AND release on
 * the same button answers it, and it answers NO by itself after 20 s (or
 * when the tank goes to sleep). The answer is one-shot: main.c takes it
 * once per frame and a YES wipes the tank. */
void touch_port_confirm_open(void);
bool touch_port_confirm_answer(int ans);       /* +1 yes / -1 no; false = no prompt up */
bool touch_port_confirm_up(void);
float touch_port_confirm_frac(void);           /* time left before it gives up, 1 -> 0 */
int  touch_port_confirm_take(void);            /* +1 / -1 once, then 0 */
bool touch_port_pressed_since(int64_t us);     /* a finger is down and landed after `us` */
bool touch_port_take_brightness_tap(void);     /* one-shot: the milestones page's brightness row was tapped */
void touch_port_set_bias(int px);              /* finger-landing correction: reported y moves up by px */
int  touch_port_bias(void);
#endif
