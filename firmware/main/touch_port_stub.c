#include "touch_port.h"
bool touch_port_init(void) { return false; }
void touch_port_poll(tank_t *t) { (void)t; }
int  touch_port_selected(void) { return -1; }
void touch_port_set_inverted(bool inverted) { (void)inverted; }
