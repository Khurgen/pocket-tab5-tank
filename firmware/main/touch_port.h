#ifndef TOUCH_PORT_H
#define TOUCH_PORT_H
#include <stdbool.h>
#include "tank.h"
bool touch_port_init(void);
void touch_port_poll(tank_t *t);
int  touch_port_selected(void);   /* tapped fish for the stats card, -1 = none */
/* milestones page: a tap ON the open stats card flips to it; it has no
 * auto-dismiss - the next tap anywhere closes it (and the card, if still up).
 * touch_port_show_milestones is the director's cue (needs no card). */
bool touch_port_milestones(void);
void touch_port_show_milestones(bool on);
void touch_port_set_inverted(bool inverted);   /* mirror coords when the screen is flipped */
#endif
