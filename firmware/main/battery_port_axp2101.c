/* battery_port_axp2101.c — battery fraction + charge state from the AXP2101
 * PMIC's fuel gauge (I2C 0x34): STATUS1 bit3 = battery present, STATUS2
 * bits[7:5] == 1 = charging, 0xA4 = state of charge in percent. Reads are
 * cached for 5 s; any I2C error or absent battery hides the meter. */
#include "battery_port.h"
#include "driver/i2c_master.h"
#include "esp_timer.h"
#include "esp_log.h"

#define AXP2101_ADDR      0x34
#define REG_STATUS1       0x00
#define REG_STATUS2       0x01
#define REG_COMMON_CFG    0x10
#define REG_BAT_PERCENT   0xA4

static i2c_master_dev_handle_t s_dev;
static int64_t s_last_us = -1;
static float s_frac; static bool s_charging, s_valid;

bool battery_port_init(i2c_master_bus_handle_t bus) {
    if (!bus || i2c_master_probe(bus, AXP2101_ADDR, 50) != ESP_OK) {
        ESP_LOGW("battery", "no AXP2101");
        return false;
    }
    i2c_device_config_t cfg = { .dev_addr_length = I2C_ADDR_BIT_LEN_7,
                                .device_address = AXP2101_ADDR, .scl_speed_hz = 400000 };
    if (i2c_master_bus_add_device(bus, &cfg, &s_dev) != ESP_OK) return false;
    ESP_LOGI("battery", "AXP2101 fuel gauge up");
    return true;
}

static bool rd(uint8_t reg, uint8_t *val) {
    return i2c_master_transmit_receive(s_dev, &reg, 1, val, 1, 100) == ESP_OK;
}

/* COMMON_CONFIG bit0 = soft power-off: every rail drops within ms, draw falls
 * to the PMIC's quiescent few uA. A PWR-button press powers the board back on. */
bool battery_port_poweroff(void) {
    if (!s_dev) return false;
    uint8_t v;
    if (!rd(REG_COMMON_CFG, &v)) return false;
    uint8_t wr[2] = { REG_COMMON_CFG, (uint8_t)(v | 0x01) };
    return i2c_master_transmit(s_dev, wr, 2, 100) == ESP_OK;
}

bool battery_port_read(float *frac, bool *charging) {
    if (!s_dev) return false;
    int64_t now = esp_timer_get_time();
    if (s_last_us < 0 || now - s_last_us > 5 * 1000000) {
        s_last_us = now;
        uint8_t st1, st2, pct;
        s_valid = rd(REG_STATUS1, &st1) && rd(REG_STATUS2, &st2) && rd(REG_BAT_PERCENT, &pct)
                  && (st1 & 0x08) && pct <= 100;          /* battery present, sane SoC */
        if (s_valid) { s_frac = pct / 100.0f; s_charging = ((st2 >> 5) & 0x07) == 0x01; }
    }
    if (!s_valid) return false;
    *frac = s_frac; *charging = s_charging;
    return true;
}
