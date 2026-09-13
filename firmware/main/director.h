/* director.h — serial "director" console: scenario setup on cue (filming,
 * bench). Lines typed on the USB serial port (the same port the log comes
 * out of) become tank commands: `hungry 3`, `feed`, `algae 40`, ... Type
 * `help` for the list. The tank owns nothing new: the console only sets
 * state the tank already has and the advisor still decides what fish do. */
#pragma once
#include "tank.h"
void director_init(void);
void director_poll(tank_t *t);   /* once per frame, from the tank task */
