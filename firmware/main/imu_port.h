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
/* handling detector (2026-09-15, for the audio port): true while the
 * device has moved within the last IMU_MOTION_HOLD_US - picked up, in a
 * hand, carried. Lying on a table it goes false. */
bool imu_port_moving(void);
int  imu_port_motion(void);                       /* last poll's movement, counts (director / tuning) */
/* drowse bracket: quiesce the accel before the panel/touch rails cut (a
 * powered chip beside rail transitions is the latch-up recipe that railed
 * X/Z on 2026-08-31 - a full power-off revived them), then soft-reset +
 * reconfigure on wake so it never resumes on trust. */
void imu_port_sleep(void);
void imu_port_wake(void);

#endif
