/* touch_port_ft3168.c — FT3168 capacitive touch (FT5x06 register family) ->
 * tank_touch_hold / tank_touch_tap, with the same gesture timing as the sim's
 * mouse: press+release < 250 ms with little movement = tap; held > 300 ms =
 * hold; a drag down from the top edge = feed at that x. Coordinates are mapped from the portrait panel to the landscape tank. */
#include "touch_port.h"
#include "board_pins.h"
#include "tank.h"
#include "esp_lcd_touch_ft5x06.h"
#include "esp_lcd_panel_io.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "driver/i2c_master.h"
#include <math.h>

static const char *TAG = "touch";
static esp_lcd_touch_handle_t s_tp;
static bool s_down; static int64_t s_press_us; static float s_px, s_py;
extern i2c_master_bus_handle_t board_i2c_bus(void);

bool touch_port_init(void) {
    esp_lcd_panel_io_handle_t io;
    esp_lcd_panel_io_i2c_config_t io_cfg = ESP_LCD_TOUCH_IO_I2C_FT5x06_CONFIG();
    io_cfg.dev_addr = I2C_ADDR_FT3168; io_cfg.scl_speed_hz = 400000;
    if (esp_lcd_new_panel_io_i2c(board_i2c_bus(), &io_cfg, &io) != ESP_OK) { ESP_LOGW(TAG, "no touch io"); return false; }
    esp_lcd_touch_config_t tp_cfg = { .x_max = PANEL_W, .y_max = PANEL_H, .rst_gpio_num = -1, .int_gpio_num = -1,
        .levels = { .reset = 0, .interrupt = 0 }, .flags = { .swap_xy = 0, .mirror_x = 0, .mirror_y = 0 } };
    if (esp_lcd_touch_new_i2c_ft5x06(io, &tp_cfg, &s_tp) != ESP_OK) { ESP_LOGW(TAG, "no FT3168"); return false; }
    ESP_LOGI(TAG, "FT3168 ready");
    return true;
}

/* call every frame from the tank task */
void touch_port_poll(tank_t *t) {
    if (!s_tp) return;
    uint16_t x[1], y[1], st[1]; uint8_t n = 0;
    esp_lcd_touch_read_data(s_tp);
    bool touched = esp_lcd_touch_get_coordinates(s_tp, x, y, st, &n, 1) && n > 0;
    int64_t now = esp_timer_get_time();
    /* portrait panel (px,py) -> landscape tank (tx,ty): tx = TANK_W-1-py, ty = px */
    float tx = touched ? (float)(TANK_W - 1 - y[0]) : s_px, ty = touched ? (float)x[0] : s_py;
    if (touched && !s_down) { s_press_us = now; s_px = tx; s_py = ty; }
    if (touched && now - s_press_us > 300000 && fabsf(ty - s_py) < 30) tank_touch_hold(t, tx, ty);
    if (!touched && s_down) {
        float dx = tx - s_px, dy = ty - s_py;
        if (now - s_press_us < 250000 && dx * dx + dy * dy < 64) tank_touch_tap(t, s_px, s_py);
        else if (s_py < 60 && dy >= 40) tank_feed(t, tx, 3);    /* drag down from the top = feed */
    }
    s_down = touched;
}
