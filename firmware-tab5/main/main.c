/*
 * Pocket Tank Tab5 v0.1.0 smoke test
 *
 * Purpose:
 *   Prove ESP32-P4 + official M5Stack Tab5 BSP + display/LVGL before
 *   integrating the Pocket Tank renderer or local language model.
 */

#include "bsp/esp-bsp.h"
#include "esp_log.h"
#include "lvgl.h"

static const char *TAG = "pocket-tab5";

void app_main(void)
{
    ESP_LOGI(TAG, "Pocket Tank Tab5 v0.1.0: setup start");

    lv_display_t *display = bsp_display_start();
    if (display == NULL) {
        ESP_LOGE(TAG, "FAIL: bsp_display_start returned NULL");
        return;
    }

    bsp_display_lock(0);

    lv_obj_t *screen = lv_screen_active();
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x071B2B), 0);

    lv_obj_t *title = lv_label_create(screen);
    lv_label_set_text(title, "POCKET TANK");
    lv_obj_set_style_text_color(title, lv_color_hex(0xE8F7FF), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_32, 0);
    lv_obj_align(title, LV_ALIGN_CENTER, 0, -40);

    lv_obj_t *status = lv_label_create(screen);
    lv_label_set_text(status, "M5Stack Tab5 / ESP32-P4\nDisplay smoke test PASS");
    lv_obj_set_style_text_color(status, lv_color_hex(0x9ED9F7), 0);
    lv_obj_set_style_text_align(status, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(status, LV_ALIGN_CENTER, 0, 35);

    bsp_display_unlock();
    bsp_display_backlight_on();

    ESP_LOGI(TAG, "PASS: BSP display initialized and LVGL screen created");
}
