/* imu_port.h — QMI8658 accelerometer -> screen orientation (180-degree flip
 * only, so the landscape tank keeps its aspect ratio either way up). */
#ifndef IMU_PORT_H
#define IMU_PORT_H
#include <stdbool.h>
#include <stdint.h>
#include "driver/i2c_master.h"

bool imu_port_init(i2c_master_bus_handle_t bus);  /* false = no IMU, never inverted */
void imu_port_poll(int64_t now_us);               /* call every frame; rate-limited inside */
bool imu_port_inverted(void);                     /* true = device is upside down */

#endif
