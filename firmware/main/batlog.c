#include "batlog.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "progression.h"     /* clock_port_now_unix: the RTC's wall clock */
#include <string.h>
#include "esp_attr.h"

/* RTC slow memory: the ring lives through deep sleep (zeroed only by a
   power-on reset), so a night's entry and wake samples sit side by side */
#define N 96
typedef struct { int64_t us; int16_t pct, mv; uint8_t bright, asleep; char why[8]; } sample_t;
RTC_DATA_ATTR static sample_t s_ring[N]; RTC_DATA_ATTR static int s_n, s_head;

void batlog_clear(void) { s_n = s_head = 0; }
void batlog_add(int pct, int mv, int bright, bool asleep, const char *why) {
    sample_t *s = &s_ring[s_head];
    /* wall clock in us (the PCF85063 sets it at every boot), so the stamp
       survives deep sleep - esp_timer restarts from zero at each wake;
       fall back to it only when the RTC is unset */
    int64_t unix = clock_port_now_unix();
    s->us = unix > 0 ? unix * 1000000LL : esp_timer_get_time();
    s->pct = (int16_t)pct; s->mv = (int16_t)mv;
    s->bright = (uint8_t)bright; s->asleep = asleep;
    strncpy(s->why, why ? why : "", sizeof s->why - 1); s->why[sizeof s->why - 1] = 0;
    s_head = (s_head + 1) % N; if (s_n < N) s_n++;
}
void batlog_print(void) {
    if (!s_n) { ESP_LOGI("batlog", "no samples yet"); return; }
    int first = (s_head - s_n + N) % N;
    const sample_t *prev = NULL; double awake_mah = 0, awake_h = 0, sleep_mah = 0, sleep_h = 0;
    int64_t t_first = s_ring[first].us;
    ESP_LOGI("batlog", "%d samples (h:mm since the first | SoC | VBAT | bright | state | mA since previous)", s_n);
    for (int k = 0; k < s_n; k++) {
        const sample_t *s = &s_ring[(first + k) % N];
        double h = (s->us - t_first) / 3.6e9;
        char cur[24] = "";
        if (prev && s->pct >= 0 && prev->pct >= 0 && s->us > prev->us) {
            double dh = (s->us - prev->us) / 3.6e9, mah = (prev->pct - s->pct) * BATLOG_CELL_MAH / 100.0;
            snprintf(cur, sizeof cur, "%6.1f mA", mah / dh);
            if (prev->asleep) { sleep_mah += mah; sleep_h += dh; } else { awake_mah += mah; awake_h += dh; }
        }
        ESP_LOGI("batlog", "  %3d:%02d | %3d%% | %4d mV | %3d | %-6s | %-6s %s", (int)h, (int)((h - (int)h) * 60),
                 s->pct, s->mv, s->bright, s->asleep ? "asleep" : "awake", s->why, cur);
        prev = s;
    }
    if (awake_h > 0) ESP_LOGI("batlog", "  awake:  %.1f h, %.0f mAh -> %.0f mA average", awake_h, awake_mah, awake_mah / awake_h);
    if (sleep_h > 0) ESP_LOGI("batlog", "  asleep: %.1f h, %.1f mAh -> %.2f mA average", sleep_h, sleep_mah, sleep_mah / sleep_h);
    ESP_LOGI("batlog", "  (SoC is 1%% = 2 mAh; short awake windows are coarse, a night is not)");
}
