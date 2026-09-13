#include "codec_port.h"
#include "esp_log.h"

static const char *TAG = "codec";
static i2c_master_dev_handle_t s_dev; static int s_addr;

static bool wr(uint8_t reg, uint8_t val) { uint8_t b[2] = { reg, val }; return i2c_master_transmit(s_dev, b, 2, 100) == ESP_OK; }
static bool rd(uint8_t reg, uint8_t *val) { return i2c_master_transmit_receive(s_dev, &reg, 1, val, 1, 100) == ESP_OK; }

void codec_port_dump(void) {
    if (!s_dev) { ESP_LOGI(TAG, "no ES8311 found"); return; }
    uint8_t r00 = 0, r01 = 0, r0d = 0, r0e = 0, r12 = 0;
    bool ok = rd(0x00, &r00) && rd(0x01, &r01) && rd(0x0D, &r0d) && rd(0x0E, &r0e) && rd(0x12, &r12);
    ESP_LOGI(TAG, "ES8311 @0x%02x %s: RESET %02x CLKMGR %02x SYS0D %02x (analog %s, vref %s, vmid %d) SYS0E %02x DAC %02x",
             s_addr, ok ? "" : "(read failed)", r00, r01, r0d,
             (r0d & 0x80) ? "down" : "ON", (r0d & 0x04) ? "ON" : "off", r0d & 3, r0e, r12);
}

bool codec_port_init(i2c_master_bus_handle_t bus) {
    if (!bus) return false;
    for (int a = 0x18; a <= 0x19; a++)                /* 0011 00x, x = the CE pin */
        if (i2c_master_probe(bus, a, 50) == ESP_OK) { s_addr = a; break; }
    if (!s_addr) { ESP_LOGW(TAG, "no ES8311 at 0x18/0x19"); return false; }
    i2c_device_config_t cfg = { .dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = s_addr, .scl_speed_hz = 400000 };
    if (i2c_master_bus_add_device(bus, &cfg, &s_dev) != ESP_OK) return false;
    uint8_t before = 0; rd(0x0D, &before);
    /* full power-down: state machine off + all blocks in reset (0x00 = the
       reset default), every clock off, every analog block down with the
       reference and vmid off, DAC down */
    bool ok = wr(0x00, 0x1F) && wr(0x01, 0x00) && wr(0x0D, 0xF8) && wr(0x12, 0x02);
    ESP_LOGI(TAG, "ES8311 @0x%02x powered down%s (SYS0D %02x -> F8)", s_addr, ok ? "" : " - a write FAILED", before);
    codec_port_dump();
    return ok;
}
