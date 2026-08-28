/* imu_port_qmi8658.c — QMI8658 6-axis IMU (I2C 0x6B, alt 0x6A) as an
 * orientation sensor: accel only at 31.25 Hz, gyro off. Polled ~4x/s from the
 * tank task; the inverted flag flips only after the gravity component along
 * the panel's landscape-vertical axis has clearly (>0.5 g) pointed the other
 * way for 3 consecutive polls, and holds its last state while the device lies
 * flat (no axis dominant), so the screen never flaps on a table. */
#include "imu_port.h"
#include "esp_log.h"

/* which accel axis is "up" when the tank is held right side up. The boot log
 * prints the live vector ("imu: g=[x y z]") — if the flip is wrong or dead,
 * hold the device upright, read which axis carries ~1 g, and fix these two. */
#define IMU_UP_AXIS 1        /* 0=X 1=Y 2=Z; calibrated 2026-08-28: upright-in-hand = -Y ~16k */
#define IMU_UP_SIGN (-1)

#define QMI8658_ADDR       0x6B
#define QMI8658_ADDR_ALT   0x6A
#define REG_WHO_AM_I       0x00   /* reads 0x05 */
#define REG_CTRL1          0x02
#define REG_CTRL2          0x03
#define REG_CTRL7          0x08
#define REG_AX_L           0x35
#define WHO_AM_I_VAL       0x05

#define POLL_INTERVAL_US   250000
#define FLIP_THRESH        8192   /* 0.5 g at +-2g full scale (16384 counts/g) */
#define FLIP_HOLD_POLLS    3      /* ~750 ms the other way up before flipping */

static const char *TAG = "imu";
static i2c_master_dev_handle_t s_dev;
static bool s_inverted;
static int s_streak;              /* consecutive polls voting for a flip */
static int64_t s_next_us;

static bool wr8(uint8_t reg, uint8_t val) {
    uint8_t buf[2] = { reg, val };
    return i2c_master_transmit(s_dev, buf, 2, 100) == ESP_OK;
}
static bool rdn(uint8_t reg, uint8_t *val, size_t n) {
    return i2c_master_transmit_receive(s_dev, &reg, 1, val, n, 100) == ESP_OK;
}

bool imu_port_init(i2c_master_bus_handle_t bus) {
    if (!bus) return false;
    uint8_t addr = QMI8658_ADDR;
    if (i2c_master_probe(bus, addr, 50) != ESP_OK) {
        addr = QMI8658_ADDR_ALT;
        if (i2c_master_probe(bus, addr, 50) != ESP_OK) { ESP_LOGW(TAG, "no QMI8658"); return false; }
    }
    i2c_device_config_t cfg = { .dev_addr_length = I2C_ADDR_BIT_LEN_7,
                                .device_address = addr, .scl_speed_hz = 400000 };
    if (i2c_master_bus_add_device(bus, &cfg, &s_dev) != ESP_OK) return false;
    uint8_t who = 0;
    if (!rdn(REG_WHO_AM_I, &who, 1) || who != WHO_AM_I_VAL) {
        ESP_LOGW(TAG, "QMI8658 whoami 0x%02x (want 0x05)", who);
        s_dev = NULL; return false;
    }
    bool ok = wr8(REG_CTRL1, 0x40)      /* address auto-increment for burst reads */
           && wr8(REG_CTRL2, 0x08)      /* accel +-2g, 31.25 Hz */
           && wr8(REG_CTRL7, 0x01);     /* accel on, gyro off */
    if (!ok) { ESP_LOGW(TAG, "QMI8658 config failed"); s_dev = NULL; return false; }
    ESP_LOGI(TAG, "QMI8658 up at 0x%02x: orientation axis %c%s", addr,
             IMU_UP_SIGN > 0 ? '+' : '-', IMU_UP_AXIS == 0 ? "X" : IMU_UP_AXIS == 1 ? "Y" : "Z");
    return true;
}

void imu_port_poll(int64_t now_us) {
    if (!s_dev || now_us < s_next_us) return;
    s_next_us = now_us + POLL_INTERVAL_US;
    uint8_t raw[6];
    if (!rdn(REG_AX_L, raw, 6)) return;
    int16_t a[3] = { (int16_t)(raw[0] | raw[1] << 8),
                     (int16_t)(raw[2] | raw[3] << 8),
                     (int16_t)(raw[4] | raw[5] << 8) };
    static int logged;
    if (logged < 3) { logged++; ESP_LOGI(TAG, "g=[%d %d %d] inverted=%d", a[0], a[1], a[2], (int)s_inverted); }
    int v = a[IMU_UP_AXIS] * IMU_UP_SIGN;
    bool wants_flip = s_inverted ? (v > FLIP_THRESH) : (v < -FLIP_THRESH);
    s_streak = wants_flip ? s_streak + 1 : 0;      /* flat / sideways: hold state */
    if (s_streak >= FLIP_HOLD_POLLS) {
        s_inverted = !s_inverted; s_streak = 0;
        ESP_LOGI(TAG, "orientation: %s", s_inverted ? "inverted" : "upright");
    }
}

bool imu_port_inverted(void) { return s_inverted; }
