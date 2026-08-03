#include "esp_log.h"
#include "esp_check.h"
#include "app_oled.h"
#include "app_ir.h"
#include "app_ui.h"

static const char *TAG = "app";

void app_main(void)
{
    ESP_LOGI(TAG, "IR signal monitor starting");

    ESP_RETURN_ON_ERROR(oled_init(), TAG, "OLED init failed");
    ESP_LOGI(TAG, "OLED ready");

    ESP_RETURN_ON_ERROR(ir_capture_init(), TAG, "IR capture init failed");
    ESP_LOGI(TAG, "IR capture ready");

    ESP_RETURN_ON_ERROR(ui_init(), TAG, "UI init failed");
    ESP_LOGI(TAG, "IR signal monitor ready");
}
