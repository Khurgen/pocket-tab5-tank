/* battery_port.h — device battery meter source (AXP2101 fuel gauge). */
#ifndef BATTERY_PORT_H
#define BATTERY_PORT_H
#include <stdbool.h>
#include "driver/i2c_master.h"

bool battery_port_init(i2c_master_bus_handle_t bus);   /* false = no PMIC, meter hidden */
bool battery_port_read(float *frac, bool *charging);   /* cached ~5 s; false = hide meter */
bool battery_port_poweroff(void);                      /* PMIC soft power-off; false = no PMIC */

#endif
