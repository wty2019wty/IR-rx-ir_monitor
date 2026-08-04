#include <string.h>
#include "app_oled.h"
#include "font5x7.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_check.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define TAG "oled"

#define OLED_I2C_ADDR   CONFIG_IR_MONITOR_OLED_I2C_ADDR
#define OLED_I2C_CLK_HZ CONFIG_IR_MONITOR_OLED_I2C_CLK_HZ
#define OLED_SDA_GPIO   CONFIG_IR_MONITOR_OLED_SDA_GPIO
#define OLED_SCL_GPIO   CONFIG_IR_MONITOR_OLED_SCL_GPIO

static uint8_t s_fb[OLED_W * OLED_H / 8];
static i2c_master_dev_handle_t s_oled_dev;

static esp_err_t oled_detect_addr(i2c_master_bus_handle_t bus, uint16_t *out_addr)
{
    /* Most SSD1315/SSD1306 modules use 0x3C; some use 0x3D (SA0=1) */
    uint16_t candidates[2] = {
        OLED_I2C_ADDR,
        (uint16_t)(OLED_I2C_ADDR ^ 0x01),
    };
    for (int i = 0; i < 2; i++) {
        esp_err_t err = i2c_master_probe(bus, candidates[i], pdMS_TO_TICKS(200));
        if (err == ESP_OK) {
            *out_addr = candidates[i];
            ESP_LOGI(TAG, "OLED detected at 0x%02X", candidates[i]);
            return ESP_OK;
        }
    }
    ESP_LOGE(TAG, "OLED NOT detected at 0x%02X / 0x%02X: check SDA/SCL wiring, VCC/GND and pull-ups",
             candidates[0], candidates[1]);
    return ESP_ERR_NOT_FOUND;
}

static esp_err_t oled_write_cmd(const uint8_t *cmds, size_t len)
{
    uint8_t pkt[16];
    if (len == 0 || len > sizeof(pkt) - 1) {
        return ESP_ERR_INVALID_ARG;
    }
    pkt[0] = 0x00; /* control byte: command stream */
    memcpy(pkt + 1, cmds, len);
    return i2c_master_transmit(s_oled_dev, pkt, len + 1, pdMS_TO_TICKS(100));
}

esp_err_t oled_init(void)
{
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = OLED_SDA_GPIO,
        .scl_io_num = OLED_SCL_GPIO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .intr_priority = 0,
        .trans_queue_depth = 0, /* synchronous mode: bus errors are visible and i2c_master_probe works */
        .flags.enable_internal_pullup = true,
    };
    i2c_master_bus_handle_t bus = NULL;
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus_cfg, &bus), TAG, "create I2C bus");

    uint16_t oled_addr;
    ESP_RETURN_ON_ERROR(oled_detect_addr(bus, &oled_addr), TAG, "OLED not found on I2C bus");

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = oled_addr,
        .scl_speed_hz = OLED_I2C_CLK_HZ,
        .scl_wait_us = 50,  /* give slave time to clock-stretch */
        .flags.disable_ack_check = false,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(bus, &dev_cfg, &s_oled_dev), TAG, "add OLED device");

    /* SSD1306/SSD1315 init, 128x64, I2C */
    uint8_t seq[2];
    uint8_t c;
    esp_err_t ret;

    c = 0xAE;            ESP_GOTO_ON_ERROR(oled_write_cmd(&c, 1), err, TAG, "display off");
    vTaskDelay(pdMS_TO_TICKS(10));
    seq[0] = 0xD5; seq[1] = 0x80; ESP_GOTO_ON_ERROR(oled_write_cmd(seq, 2), err, TAG, "clock divide");
    seq[0] = 0xA8; seq[1] = 0x3F; ESP_GOTO_ON_ERROR(oled_write_cmd(seq, 2), err, TAG, "multiplex");
    seq[0] = 0xD3; seq[1] = 0x00; ESP_GOTO_ON_ERROR(oled_write_cmd(seq, 2), err, TAG, "display offset");
    c = 0x40;          ESP_GOTO_ON_ERROR(oled_write_cmd(&c, 1), err, TAG, "start line");
    seq[0] = 0x8D; seq[1] = 0x14; ESP_GOTO_ON_ERROR(oled_write_cmd(seq, 2), err, TAG, "charge pump");
    vTaskDelay(pdMS_TO_TICKS(10));
    seq[0] = 0x20; seq[1] = 0x00; ESP_GOTO_ON_ERROR(oled_write_cmd(seq, 2), err, TAG, "addressing mode");
    c = 0xA1;          ESP_GOTO_ON_ERROR(oled_write_cmd(&c, 1), err, TAG, "segment remap");
    c = 0xC8;          ESP_GOTO_ON_ERROR(oled_write_cmd(&c, 1), err, TAG, "COM scan");
    seq[0] = 0xDA; seq[1] = 0x12; ESP_GOTO_ON_ERROR(oled_write_cmd(seq, 2), err, TAG, "COM pins");
    seq[0] = 0x81; seq[1] = 0xCF; ESP_GOTO_ON_ERROR(oled_write_cmd(seq, 2), err, TAG, "contrast");
    seq[0] = 0xD9; seq[1] = 0xF1; ESP_GOTO_ON_ERROR(oled_write_cmd(seq, 2), err, TAG, "pre-charge");
    seq[0] = 0xDB; seq[1] = 0x40; ESP_GOTO_ON_ERROR(oled_write_cmd(seq, 2), err, TAG, "VCOMH");
    c = 0xA4;          ESP_GOTO_ON_ERROR(oled_write_cmd(&c, 1), err, TAG, "display RAM");
    c = 0xA6;          ESP_GOTO_ON_ERROR(oled_write_cmd(&c, 1), err, TAG, "normal mode");
    c = 0xAF;          ESP_GOTO_ON_ERROR(oled_write_cmd(&c, 1), err, TAG, "display on");
    vTaskDelay(pdMS_TO_TICKS(50));

    memset(s_fb, 0xFF, sizeof(s_fb)); /* Fill white for test */
    oled_flush();
    vTaskDelay(pdMS_TO_TICKS(500));
    memset(s_fb, 0, sizeof(s_fb)); /* Clear */
    oled_flush();
    return ESP_OK;

err:
    return ret;
}

void oled_flush(void)
{
    uint8_t cmds[] = {
        0x20, 0x00,             /* horizontal addressing mode */
        0x21, 0x00, 0x7F,       /* column 0..127 */
        0x22, 0x00, 0x07,       /* page 0..7 */
    };
    oled_write_cmd(cmds, sizeof(cmds));

    for (int i = 0; i < (int)sizeof(s_fb); i += 128) {
        uint8_t pkt[129];
        pkt[0] = 0x40; /* control byte: data stream */
        memcpy(pkt + 1, s_fb + i, 128);
        i2c_master_transmit(s_oled_dev, pkt, sizeof(pkt), pdMS_TO_TICKS(100));
    }
}

void oled_clear(void)
{
    memset(s_fb, 0, sizeof(s_fb));
}

void oled_pixel(int x, int y, bool on)
{
    if (x < 0 || x >= OLED_W || y < 0 || y >= OLED_H) {
        return;
    }
    uint8_t mask = (uint8_t)(1u << (y % 8));
    uint8_t *px = &s_fb[(y / 8) * OLED_W + x];
    if (on) {
        *px |= mask;
    } else {
        *px &= (uint8_t)~mask;
    }
}

void oled_fill_rect(int x0, int y0, int x1, int y1, bool on)
{
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 >= OLED_W) x1 = OLED_W - 1;
    if (y1 >= OLED_H) y1 = OLED_H - 1;
    for (int y = y0; y <= y1; y++) {
        for (int x = x0; x <= x1; x++) {
            oled_pixel(x, y, on);
        }
    }
}

void oled_invert_rect(int x0, int y0, int x1, int y1)
{
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 >= OLED_W) x1 = OLED_W - 1;
    if (y1 >= OLED_H) y1 = OLED_H - 1;
    for (int y = y0; y <= y1; y++) {
        uint8_t mask = (uint8_t)(1u << (y % 8));
        for (int x = x0; x <= x1; x++) {
            s_fb[(y / 8) * OLED_W + x] ^= mask;
        }
    }
}

void oled_draw_char(int x, int y, char c, bool invert)
{
    const uint8_t *g = font5x7_glyph(c);
    for (int col = 0; col < FONT5X7_CH_W; col++) {
        for (int row = 0; row < FONT5X7_CH_H; row++) {
            bool on = (g[col] >> row) & 1;
            if (invert) {
                on = !on;
            }
            oled_pixel(x + col, y + row, on);
        }
    }
}

void oled_draw_text(int x, int y, const char *s, bool invert)
{
    while (*s) {
        oled_draw_char(x, y, *s, invert);
        x += FONT5X7_CH_ADVANCE;
        s++;
    }
}

void oled_draw_text_center(int y, const char *s, bool invert)
{
    int w = oled_text_width(s);
    int x = (OLED_W - w) / 2;
    if (x < 0) {
        x = 0;
    }
    oled_draw_text(x, y, s, invert);
}

int oled_text_width(const char *s)
{
    return font5x7_text_width(s);
}
