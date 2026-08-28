#ifndef TOUCH_PORT_H
#define TOUCH_PORT_H
#include <stdbool.h>
#include "tank.h"
bool touch_port_init(void);
void touch_port_poll(tank_t *t);
int  touch_port_selected(void);   /* tapped fish for the stats card, -1 = none */
void touch_port_set_inverted(bool inverted);   /* mirror coords when the screen is flipped */
#endif
