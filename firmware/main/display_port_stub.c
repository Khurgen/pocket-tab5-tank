/* display_port_stub.c — no panel (QEMU / compile-only). Counts frames so the
 * render loop is exercised end to end and reports fps in the log. */
#include "display_port.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "display";
static uint32_t frames = 0;
static int64_t last_report = 0;

bool display_port_init(void) { ESP_LOGI(TAG, "stub display port (no panel)"); return true; }
void display_port_sleep(void) {}
void display_port_set_inverted(bool inverted) { (void)inverted; }

void display_port_flush(const uint16_t *fb) {
    (void)fb;
    frames++;
    int64_t now = esp_timer_get_time();
    if (now - last_report > 5 * 1000000) {
        if (last_report) ESP_LOGI(TAG, "render %.1f fps", frames / ((now - last_report) / 1e6));
        frames = 0; last_report = now;
    }
}
