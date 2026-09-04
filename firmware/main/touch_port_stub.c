#include "touch_port.h"
bool touch_port_init(void) { return false; }
void touch_port_poll(tank_t *t) { (void)t; }
int  touch_port_selected(void) { return -1; }
static bool s_ms;
bool touch_port_milestones(void) { return s_ms; }
void touch_port_show_milestones(bool on) { s_ms = on; }
void touch_port_set_inverted(bool inverted) { (void)inverted; }
