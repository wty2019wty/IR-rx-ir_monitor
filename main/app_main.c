#include "esp_log.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "app_oled.h"
#include "app_ir.h"
#include "app_ui.h"

static const char *TAG = "app";

static void show_status(const char *line1, const char *line2)
{
    oled_clear();
    oled_draw_text_center(20, line1, false);
    if (line2) {
        oled_draw_text_center(32, line2, false);
    }
    oled_flush();
}

void app_main(void)
{
    ESP_LOGI(TAG, "IR signal monitor starting");

    esp_err_t err = oled_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OLED init failed: %s", esp_err_to_name(err));
        return;
    }
    ESP_LOGI(TAG, "OLED ready");

    show_status("IR MONITOR", "Initializing...");
    vTaskDelay(pdMS_TO_TICKS(500));

    /* Initialize IR capture (creates RMT channels but does NOT start receiving) */
    show_status("IR MONITOR", "Init RMT...");
    err = ir_capture_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "IR capture init failed: %s", esp_err_to_name(err));
        show_status("RMT FAILED!", esp_err_to_name(err));
        return;
    }
    ESP_LOGI(TAG, "IR capture ready");

    /* Initialize storage BEFORE starting RMT receive to avoid flash/RMT ISR conflicts */
    show_status("IR MONITOR", "Init Storage...");
    err = ir_storage_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "IR storage init failed: %s", esp_err_to_name(err));
        show_status("STORAGE FAILED!", esp_err_to_name(err));
        return;
    }
    ESP_LOGI(TAG, "IR storage ready");

    /* Now safe to start RMT receiving (after SPIFFS init completes) */
    show_status("IR MONITOR", "Start RX...");
    err = ir_start_receive();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "IR start receive failed: %s", esp_err_to_name(err));
        show_status("RX FAILED!", esp_err_to_name(err));
        return;
    }

    show_status("IR MONITOR", "Starting UI...");
    err = ui_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "UI init failed: %s", esp_err_to_name(err));
        show_status("UI FAILED!", esp_err_to_name(err));
        return;
    }
    ESP_LOGI(TAG, "IR signal monitor ready");
}
