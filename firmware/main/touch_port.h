#ifndef TOUCH_PORT_H
#define TOUCH_PORT_H
#include <stdbool.h>
#include "tank.h"
bool touch_port_init(void);
void touch_port_poll(tank_t *t);
#endif
