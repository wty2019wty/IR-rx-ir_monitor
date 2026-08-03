#include "esp_log.h"
#include "esp_err.h"
#include "app_oled.h"
#include "app_ir.h"
#include "app_ui.h"

static const char *TAG = "app";

void app_main(void)
{
    ESP_LOGI(TAG, "IR signal monitor starting");

    esp_err_t err = oled_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OLED init failed: %s", esp_err_to_name(err));
        return;
    }
    ESP_LOGI(TAG, "OLED ready");

    err = ir_capture_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "IR capture init failed: %s", esp_err_to_name(err));
        return;
    }
    ESP_LOGI(TAG, "IR capture ready");

    err = ui_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "UI init failed: %s", esp_err_to_name(err));
        return;
    }
    ESP_LOGI(TAG, "IR signal monitor ready");
}
