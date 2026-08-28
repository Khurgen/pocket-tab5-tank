/* main.c — pocket-tank firmware entry (ESP32-S3).
 *   core 0: tank reflex layer + render at 60 fps, frames to the display port
 *   core 1: LLM advisor (q4_model over the mmap'd flash model partition)
 * Boot: assert the PSRAM plan, mmap the model partition, start both loops.
 * Without a panel (QEMU / bring-up) the display port is a counting stub and
 * every decision + tok/s goes to the log. */
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "tank.h"
#include "advisor.h"
#include "render.h"
#include "psram_plan.h"
#include "display_port.h"
#include "advisor_llm_esp.h"
#include "touch_port.h"
#include "battery_port.h"
#include "imu_port.h"
#include "progression.h"
#include "nvs_flash.h"
#include "rtc_port.h"
#include "driver/i2c_master.h"
#include "esp_async_memcpy.h"
#include "freertos/semphr.h"
#include "driver/gpio.h"
#include "esp_sleep.h"

/* sleep button: BOOT (GPIO0, active low, RTC-wake capable). A press saves the
 * tank, powers the panel down and deep-sleeps; the next press wakes through a
 * normal boot, so progression's absence rules (RTC ravenous etc.) just apply. */
#define BTN_SLEEP GPIO_NUM_0
#ifdef CONFIG_POCKET_TANK_DISPLAY_SH8601
extern i2c_master_bus_handle_t board_i2c_bus(void);
#else
static i2c_master_bus_handle_t board_i2c_bus(void) { return NULL; }
#endif

/* tokenizer.bin is tiny: embed it in the app image */
extern const uint8_t tokenizer_bin_start[] asm("_binary_tokenizer_bin_start");
extern const uint8_t tokenizer_bin_end[]   asm("_binary_tokenizer_bin_end");

static const char *TAG = "pocket-tank";
static tank_t tank;
static uint16_t *fb[PLAN_FB_COUNT];
static bool llm_ok = false;

/* GDMA prefetch of the static scene into the idle framebuffer: overlaps the
 * 320 KB scene restore with tank logic + the frame sleep instead of a CPU
 * memcpy inside render_tank. */
static async_memcpy_handle_t s_amc;
static SemaphoreHandle_t s_amc_done;
static bool s_prefetch_pending; static uint16_t *s_prefetch_fb; static unsigned s_prefetch_ep;

static bool amc_cb(async_memcpy_handle_t h, async_memcpy_event_t *ev, void *ctx) {
    (void)h; (void)ev; (void)ctx;
    BaseType_t hp = pdFALSE;
    xSemaphoreGiveFromISR(s_amc_done, &hp);
    return hp == pdTRUE;
}

static void assert_plan(void) {
    size_t psram = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    size_t free_ps = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    size_t free_in = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    ESP_LOGI(TAG, "PSRAM total %u KB free %u KB | internal free %u KB",
             (unsigned)psram / 1024, (unsigned)free_ps / 1024, (unsigned)free_in / 1024);
    size_t need = PLAN_FB_TOTAL + PLAN_KV_BYTES + PLAN_ACT_BYTES + PLAN_PSRAM_MIN_FREE_AFTER;
    if (free_ps < need)
        ESP_LOGW(TAG, "PSRAM plan NOT met (need %u KB, have %u KB) - degraded mode (QEMU?)",
                 (unsigned)need / 1024, (unsigned)free_ps / 1024);
    else
        ESP_LOGI(TAG, "PSRAM plan OK (need %u KB)", (unsigned)need / 1024);
}

static void enter_sleep(void) {
    ESP_LOGI(TAG, "sleep: saving tank, panel off, deep sleep (BOOT wakes)");
    progression_save(&tank);
    display_port_sleep();
    vTaskDelay(pdMS_TO_TICKS(50));
    /* ext0 wake is LEVEL-triggered: arming it while the finger still holds
     * GPIO0 low wakes the chip the instant it sleeps (the old "screen pops
     * back on" lottery — it depended on press length). Never sleep held. */
    while (!gpio_get_level(BTN_SLEEP)) vTaskDelay(pdMS_TO_TICKS(10));
    vTaskDelay(pdMS_TO_TICKS(20));
    esp_sleep_enable_ext0_wakeup(BTN_SLEEP, 0);
    esp_deep_sleep_start();
}

/* full power-down: save, then the AXP2101 cuts every rail (~ its own quiescent
 * uA until the PWR button boots it). Deep sleep keeps the module + touch +
 * codec rails up, so this is the mode for shelving the tank for weeks. */
static void enter_poweroff(void) {
    ESP_LOGI(TAG, "power-off: saving tank, PMIC soft cut (PWR button boots)");
    progression_save(&tank);
    display_port_sleep();
    vTaskDelay(pdMS_TO_TICKS(50));
    if (battery_port_poweroff()) vTaskDelay(pdMS_TO_TICKS(500));  /* rails drop here */
    enter_sleep();   /* no PMIC (QEMU / bring-up) or write failed: deep sleep */
}

/* armed only after the button has been seen released, so the press that woke
 * the chip doesn't put it straight back to sleep. Short press = sleep, acted
 * on at RELEASE (see enter_sleep); held >= 1.5 s = full power-off. */
#define BTN_DEBOUNCE_US 50000
#define BTN_LONG_US     1500000
static void sleep_button_poll(int64_t now) {
    static bool armed; static int64_t low_since;
    if (gpio_get_level(BTN_SLEEP)) {
        if (armed && low_since && now - low_since >= BTN_DEBOUNCE_US) enter_sleep();
        armed = true; low_since = 0;
    } else if (armed) {
        if (!low_since) low_since = now;
        else if (now - low_since >= BTN_LONG_US) enter_poweroff();
    }
}

static void tank_task(void *arg) {
    (void)arg;
    int64_t last = esp_timer_get_time(); int cur = 0;
    int64_t last_log = last;
    int64_t render_us = 0, flush_us = 0; uint32_t frames = 0;
    for (;;) {
        int64_t now = esp_timer_get_time();
        float dt = (now - last) / 1e6f; last = now; if (dt > 0.25f) dt = 0.25f;
        sleep_button_poll(now);
        imu_port_poll(now);
        bool inv = imu_port_inverted();
        display_port_set_inverted(inv);   /* per-frame, so a flip lands between flushes */
        touch_port_set_inverted(inv);
        touch_port_poll(&tank);
        tank_tick(&tank, dt, llm_ok ? advisor_llm_esp : advisor_rules);
        progression_tick(&tank, dt);
        if (fb[cur]) {
            if (s_prefetch_pending) {                       /* prior frame's scene prefetch */
                xSemaphoreTake(s_amc_done, portMAX_DELAY);
                s_prefetch_pending = false;
                render_fb_primed(s_prefetch_fb, s_prefetch_ep);
            }
            int64_t t0 = esp_timer_get_time();
            render_tank(&tank, fb[cur], TANK_W);
            touch_port_poll(&tank);          /* the CST816 is polled, not interrupt-
                                                driven: extra samples inside the frame
                                                keep quick finger taps from slipping
                                                between 40 ms frame boundaries */
            int sel = touch_port_selected();
            if (sel >= 0) {                      /* tapped fish: stats card + battery */
                render_stats_card(&tank, sel, fb[cur], TANK_W);
                float bf; bool chg;
                if (battery_port_read(&bf, &chg)) render_battery(fb[cur], TANK_W, bf, chg);
            }
            int64_t t1 = esp_timer_get_time();
            display_port_flush(fb[cur]);
            touch_port_poll(&tank);
            render_us += t1 - t0; flush_us += esp_timer_get_time() - t1; frames++;
            uint16_t *next = fb[cur ^ 1];
            const uint16_t *scene = render_scene_buf(&s_prefetch_ep);
            if (s_amc && scene && next && next != fb[cur] &&
                esp_async_memcpy(s_amc, next, (void *)scene, PLAN_FB_BYTES, amc_cb, NULL) == ESP_OK) {
                s_prefetch_fb = next; s_prefetch_pending = true;
            }
        }
        cur ^= 1;
        if (now - last_log > 10 * 1000000) {
            if (frames) {
                unsigned ep; const uint16_t *sc = render_scene_buf(&ep);
                ESP_LOGI("display", "fb mid 0x%04x corner 0x%04x | scene mid 0x%04x ep %u | night %d prefetch %d",
                         fb[cur] ? fb[cur][(TANK_H / 2) * TANK_W + TANK_W / 2] : 0,
                         fb[cur] ? fb[cur][5 * TANK_W + 5] : 0,
                         sc ? sc[(TANK_H / 2) * TANK_W + TANK_W / 2] : 0, ep,
                         (int)tank.night, (int)s_prefetch_pending);
                ESP_LOGI("display", "%.1f fps | render %.1f ms flush %.1f ms | scene %.1f shafts %.1f veg %.1f fd/bub %.1f fish %.1f vig %.1f",
                         frames * 1e6f / (float)(now - last_log),
                         render_us / 1e3f / frames, flush_us / 1e3f / frames,
                         render_prof_us[0] / 1e3f / frames, render_prof_us[1] / 1e3f / frames,
                         render_prof_us[2] / 1e3f / frames, render_prof_us[3] / 1e3f / frames,
                         render_prof_us[4] / 1e3f / frames, render_prof_us[5] / 1e3f / frames);
                memset(render_prof_us, 0, sizeof render_prof_us);
            }
            render_us = flush_us = 0; frames = 0;
            uint32_t d, ms; float tps; advisor_llm_esp_stats(&d, &ms, &tps);
            char goals[N_FISH_MAX * 16] = ""; size_t gl = 0;
            for (int i = 0; i < tank.n_fish && gl + 16 < sizeof goals; i++)
                gl += snprintf(goals + gl, sizeof goals - gl, "%s%s", i ? " " : "", GOAL_NAMES[tank.fish[i].goal.id]);
            ESP_LOGI(TAG, "t=%.0fs %d fish goals: %s | asks %lu decisions %lu last %lu ms %.1f tok/s | starve-ignored %d | heap int %u KB psram %u KB",
                     tank.clock, tank.n_fish, goals, (unsigned long)tank.advisor_asks,
                     (unsigned long)d, (unsigned long)ms, tps, tank_reflex_overrides,
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024,
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024);
            last_log = now;
        }
        int spent_ms = (int)((esp_timer_get_time() - now) / 1000);
        int rest = 16 - spent_ms;                /* pace toward 60 fps, always yield >= 1 tick */
        vTaskDelay(pdMS_TO_TICKS(rest < 1 ? 1 : rest));
    }
}

void app_main(void) {
    ESP_LOGI(TAG, "pocket-tank boot%s",
             esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_EXT0 ? " (woken by button)" : "");
    gpio_config_t btn = { .pin_bit_mask = 1ULL << BTN_SLEEP, .mode = GPIO_MODE_INPUT,
                          .pull_up_en = GPIO_PULLUP_ENABLE };
    gpio_config(&btn);
    if (nvs_flash_init() != ESP_OK) { nvs_flash_erase(); nvs_flash_init(); }
    assert_plan();
    for (int i = 0; i < PLAN_FB_COUNT; i++) {
        fb[i] = heap_caps_aligned_alloc(64, PLAN_FB_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!fb[i]) fb[i] = i ? fb[0] : NULL;          /* no PSRAM: share or skip */
    }
    if (!fb[0]) ESP_LOGW(TAG, "no framebuffer RAM: rendering disabled (tank still runs)");
    /* static-scene cache: gradient/pebbles/reef drawn once per lighting state */
    uint16_t *scene = heap_caps_aligned_alloc(64, PLAN_FB_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (scene) render_set_scene_cache(scene); else ESP_LOGW(TAG, "no scene cache RAM: full redraw per frame");
    uint8_t *vig = heap_caps_malloc(TANK_W * TANK_H, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (vig) render_set_vignette_cache(vig);
    /* model: mmap the raw partition; weights are read through the flash cache */
    const esp_partition_t *mp = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, 0x40, "model");
    if (!mp) { ESP_LOGE(TAG, "no model partition"); }
    else {
        const void *map; esp_partition_mmap_handle_t h;
        if (esp_partition_mmap(mp, 0, mp->size, ESP_PARTITION_MMAP_DATA, &map, &h) == ESP_OK) {
            llm_ok = advisor_llm_esp_init(map, mp->size, tokenizer_bin_start,
                                          tokenizer_bin_end - tokenizer_bin_start);
            ESP_LOGI(TAG, "model partition %u KB mmap'd, advisor %s", (unsigned)mp->size / 1024, llm_ok ? "LLM" : "rules (model missing)");
        } else ESP_LOGE(TAG, "model mmap failed");
    }
    render_clock_us = esp_timer_get_time;    /* per-stage frame profiling in the display log */
    display_port_init();
    touch_port_init();
    battery_port_init(board_i2c_bus());
    imu_port_init(board_i2c_bus());   /* screen auto-flip; absent IMU = always upright */
    /* scene-prefetch DMA: installed only AFTER the display grabbed its SPI DMA
       channel — installed earlier, async memcpy steals SPI2's GDMA trigger
       slot and the panel silently loses its pixel path (black screen). */
    if (scene && fb[0] && fb[1] != fb[0]) {
        async_memcpy_config_t amc_cfg = ASYNC_MEMCPY_DEFAULT_CONFIG();
        amc_cfg.backlog = 4; amc_cfg.sram_trans_align = 4; amc_cfg.psram_trans_align = 64;
        s_amc_done = xSemaphoreCreateBinary();
        if (esp_async_memcpy_install(&amc_cfg, &s_amc) != ESP_OK) {
            s_amc = NULL; ESP_LOGW(TAG, "async memcpy unavailable: CPU scene restore");
        }
    }
    rtc_port_init(board_i2c_bus());   /* wall clock for the ravenous rule */
    tank_init(&tank, (uint32_t)esp_timer_get_time() ^ 0xC0FFEEu);
    progression_boot(&tank);                 /* restore (or a new random pair) + ravenous rule */
    ESP_LOGI(TAG, "population %d (cap %d): %s + %s ...", tank.n_fish, POP_CAP,
             tank.fish[0].name, tank.n_fish > 1 ? tank.fish[1].name : "-");
    xTaskCreatePinnedToCore(tank_task, "tank", 12288, NULL, 4, NULL, 0);
}
