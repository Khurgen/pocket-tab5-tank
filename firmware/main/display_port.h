/* display_port.h — the ONLY platform-specific seam for the renderer.
 * The tank renders into an RGB565 buffer (common/render.c); this port ships
 * it to the panel. QEMU/bring-up: stub. Track 4: SH8601 over QSPI (V1 board)
 * or CO5300 (V2), rotated 90 degrees so the tank is landscape 448x368. */
#ifndef DISPLAY_PORT_H
#define DISPLAY_PORT_H
#include <stdint.h>
#include <stdbool.h>

bool display_port_init(void);
/* push a full TANK_W x TANK_H RGB565 frame; may return before DMA completes */
void display_port_flush(const uint16_t *fb);
/* power the panel down for device sleep; a later boot re-inits it */
void display_port_sleep(void);
/* true = present the frame rotated 180 degrees (device held upside down) */
void display_port_set_inverted(bool inverted);
#endif
