#include <stdio.h>
#include "app_ui.h"
#include "app_oled.h"
#include "app_ir.h"
#include "font5x7.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

#define TAG "ui"

#define MENU_ITEMS 2

typedef enum {
    SCR_MENU,
    SCR_RAW,
    SCR_NEC,
} screen_t;

typedef struct {
    gpio_num_t gpio;
    bool last;
    bool busy;
} btn_t;

static const char *const s_menu_items[MENU_ITEMS] = {
    "RAW SIGNAL FEATURES",
    "NEC DECODE",
};

static btn_t s_btn_up =   { CONFIG_IR_MONITOR_BTN_UP_GPIO,   false, false };
static btn_t s_btn_down = { CONFIG_IR_MONITOR_BTN_DOWN_GPIO, false, false };
static btn_t s_btn_ok =   { CONFIG_IR_MONITOR_BTN_OK_GPIO,   false, false };
static btn_t s_btn_back = { CONFIG_IR_MONITOR_BTN_BACK_GPIO, false, false };

static screen_t s_screen = SCR_MENU;
static int s_menu_sel = 0;
static bool s_paused = false;
static ir_frame_t s_disp;

static void buttons_init(void)
{
    gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << CONFIG_IR_MONITOR_BTN_UP_GPIO) |
                        (1ULL << CONFIG_IR_MONITOR_BTN_DOWN_GPIO) |
                        (1ULL << CONFIG_IR_MONITOR_BTN_OK_GPIO) |
                        (1ULL << CONFIG_IR_MONITOR_BTN_BACK_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&cfg);
}

static bool btn_pressed(btn_t *b)
{
    bool cur = (gpio_get_level(b->gpio) == 0); /* active low */
    bool pressed = cur && !b->last;
    b->last = cur;
    if (!cur) {
        b->busy = false;
    }
    if (pressed && !b->busy) {
        b->busy = true;
        return true;
    }
    return false;
}

static const char *raw_hint(void)
{
    if (s_disp.nec_ok && s_disp.nec_repeat) {
        return "NEC REPEAT";
    }
    if (s_disp.nec_ok && s_disp.nec_bits == 32) {
        return s_disp.nec_chksum_ok ? "NEC 32BIT OK" : "NEC 32BIT";
    }
    if (s_disp.seg_count > 2 && s_disp.total_us > 30000 && s_disp.total_us < 90000) {
        return "NEC-LIKE?";
    }
    if (s_disp.seg_count < 2) {
        return "NOISE?";
    }
    return "UNKNOWN";
}

static void draw_mode_header(const char *mode)
{
    char line[40];
    snprintf(line, sizeof(line), "%s %s #%04lu",
             mode, s_paused ? "PAUSE" : "LIVE ", (unsigned long)s_disp.seq);
    oled_draw_text(0, 0, line, false);
}

static void draw_menu(void)
{
    oled_clear();
    oled_draw_text_center(0, "IR SIGNAL MONITOR", false);

    for (int i = 0; i < MENU_ITEMS; i++) {
        int y = 16 + i * 10;
        bool sel = (i == s_menu_sel);
        if (sel) {
            oled_fill_rect(0, y, OLED_W - 1, y + FONT5X7_CH_H, true);
        }
        oled_draw_text(6, y, s_menu_items[i], sel);
    }

    oled_draw_text_center(56, "OK:Enter  BK:Back", false);
    oled_flush();
}

static void draw_raw(void)
{
    oled_clear();
    draw_mode_header("RAW");

    if (!s_disp.valid) {
        oled_draw_text_center(24, "WAITING FOR IR...", false);
        oled_draw_text_center(40, "POINT REMOTE AT RX", false);
    } else {
        char line[40];
        snprintf(line, sizeof(line), "EDGE %lu  DUR %lu.%02lums",
                 (unsigned long)s_disp.seg_count,
                 (unsigned long)(s_disp.total_us / 1000),
                 (unsigned long)((s_disp.total_us % 1000) / 10));
        oled_draw_text(0, 8, line, false);

        snprintf(line, sizeof(line), "LEAD %lu %luus",
                 (unsigned long)s_disp.leader_pulse_us,
                 (unsigned long)s_disp.leader_space_us);
        oled_draw_text(0, 16, line, false);

        snprintf(line, sizeof(line), "BURST %lu  GAP %luus",
                 (unsigned long)s_disp.pulse_count,
                 (unsigned long)s_disp.last_gap_us);
        oled_draw_text(0, 24, line, false);

        snprintf(line, sizeof(line), "MIN %lu  MAX %luus",
                 (unsigned long)s_disp.min_pulse_us,
                 (unsigned long)s_disp.max_pulse_us);
        oled_draw_text(0, 32, line, false);

        snprintf(line, sizeof(line), "HINT %s", raw_hint());
        oled_draw_text(0, 40, line, false);
    }

    oled_draw_text_center(56, "OK:Pause  BK:Back", false);
    oled_flush();
}

static void draw_nec(void)
{
    oled_clear();
    draw_mode_header("NEC");

    if (!s_disp.valid) {
        oled_draw_text_center(24, "WAITING FOR NEC...", false);
        oled_draw_text_center(40, "POINT REMOTE AT RX", false);
    } else if (s_disp.nec_ok && s_disp.nec_repeat) {
        char line[40];
        snprintf(line, sizeof(line), "REPEAT CODE");
        oled_draw_text(0, 8, line, false);
        snprintf(line, sizeof(line), "ADDR 0x%04X", (unsigned)s_disp.nec_addr);
        oled_draw_text(0, 16, line, false);
        snprintf(line, sizeof(line), "CMD 0x%02X", (unsigned)(s_disp.nec_cmd & 0xFF));
        oled_draw_text(0, 24, line, false);
        oled_draw_text(0, 32, "KEY HELD DOWN", false);
    } else if (s_disp.nec_ok) {
        char line[40];
        if (s_disp.nec_ext_addr) {
            snprintf(line, sizeof(line), "ADDR16 0x%04X CMD 0x%02X",
                     (unsigned)s_disp.nec_addr, (unsigned)(s_disp.nec_cmd & 0xFF));
        } else {
            snprintf(line, sizeof(line), "ADDR 0x%02X  CMD 0x%02X",
                     (unsigned)(s_disp.nec_addr & 0xFF), (unsigned)(s_disp.nec_cmd & 0xFF));
        }
        oled_draw_text(0, 8, line, false);

        snprintf(line, sizeof(line), "RAW %08lX", (unsigned long)s_disp.nec_raw);
        oled_draw_text(0, 16, line, false);

        if (s_disp.nec_ext_addr) {
            snprintf(line, sizeof(line), "16-BIT ADDR");
        } else {
            snprintf(line, sizeof(line), "%s", s_disp.nec_chksum_ok ? "CHKSUM OK" : "CHKSUM ERR");
        }
        oled_draw_text(0, 24, line, false);
    } else {
        char line[40];
        snprintf(line, sizeof(line), "NOT NEC  EDGE %lu", (unsigned long)s_disp.seg_count);
        oled_draw_text(0, 8, line, false);
        snprintf(line, sizeof(line), "DUR %lu.%02lums",
                 (unsigned long)(s_disp.total_us / 1000),
                 (unsigned long)((s_disp.total_us % 1000) / 10));
        oled_draw_text(0, 16, line, false);
        oled_draw_text(0, 24, "SEE RAW MODE", false);
    }

    oled_draw_text_center(56, "OK:Pause  BK:Back", false);
    oled_flush();
}

static void draw_screen(void)
{
    switch (s_screen) {
    case SCR_MENU:
        draw_menu();
        break;
    case SCR_RAW:
        draw_raw();
        break;
    case SCR_NEC:
        draw_nec();
        break;
    }
}

static void handle_buttons(void)
{
    bool redraw = false;

    if (btn_pressed(&s_btn_up)) {
        if (s_screen == SCR_MENU) {
            s_menu_sel = (s_menu_sel + MENU_ITEMS - 1) % MENU_ITEMS;
            redraw = true;
        }
    }
    if (btn_pressed(&s_btn_down)) {
        if (s_screen == SCR_MENU) {
            s_menu_sel = (s_menu_sel + 1) % MENU_ITEMS;
            redraw = true;
        }
    }
    if (btn_pressed(&s_btn_ok)) {
        if (s_screen == SCR_MENU) {
            s_screen = (s_menu_sel == 0) ? SCR_RAW : SCR_NEC;
            s_paused = false;
        } else {
            s_paused = !s_paused;
        }
        redraw = true;
    }
    if (btn_pressed(&s_btn_back)) {
        if (s_screen != SCR_MENU) {
            s_screen = SCR_MENU;
            s_paused = false;
        }
        redraw = true;
    }

    if (redraw) {
        draw_screen();
    }
}

static void ui_task(void *arg)
{
    buttons_init();
    draw_screen();

    for (;;) {
        handle_buttons();
        if (!s_paused) {
            ir_frame_t f;
            if (ir_get_frame(&f)) {
                s_disp = f;
                if (s_screen == SCR_RAW || s_screen == SCR_NEC) {
                    draw_screen();
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

esp_err_t ui_init(void)
{
    if (xTaskCreate(ui_task, "ui_task", 4096, NULL, 5, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
