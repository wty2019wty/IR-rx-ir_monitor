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

#define MENU_ITEMS 4
#define MAX_VISIBLE_ITEMS 4
#define PLAYBACK_REPEAT_COUNT 3

typedef enum {
    SCR_MENU,
    SCR_RAW,
    SCR_NEC,
    SCR_PLAYBACK,
    SCR_STORAGE,
    SCR_DELETE_CONFIRM,
    SCR_DELETE_SELECT,
} screen_t;

typedef struct {
    gpio_num_t gpio;
    bool last;
    bool busy;
} btn_t;

static const char *const s_menu_items[MENU_ITEMS] = {
    "RAW MONITOR",
    "NEC MONITOR",
    "PLAYBACK",
    "STORAGE MGR",
};

static btn_t s_btn_up =   { CONFIG_IR_MONITOR_BTN_UP_GPIO,   false, false };
static btn_t s_btn_down = { CONFIG_IR_MONITOR_BTN_DOWN_GPIO, false, false };
static btn_t s_btn_ok =   { CONFIG_IR_MONITOR_BTN_OK_GPIO,   false, false };
static btn_t s_btn_back = { CONFIG_IR_MONITOR_BTN_BACK_GPIO, false, false };

static screen_t s_screen = SCR_MENU;
static int s_menu_sel = 0;
static bool s_paused = false;
static ir_frame_t s_disp;

/* Save state */
static bool s_just_saved = false;
static uint32_t s_save_msg_time = 0;

/* Playback list state */
static int s_playback_sel = 0;
static uint32_t s_playback_count = 0;
static ir_recording_info_t s_playback_list[IR_MAX_RECORDINGS];  /* Store all recordings info */
static bool s_playback_playing = false;
static uint32_t s_playback_repeat = 0;

/* Storage manager state */
static int s_storage_sel = 0;
static int s_delete_sel = 0;

/* Scroll position for list pages */
static int s_pb_start = 0;
static int s_del_start = 0;

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

static void load_playback_list(void)
{
    s_playback_count = ir_get_saved_recording_count();
    for (int i = 0; i < (int)s_playback_count && i < IR_MAX_RECORDINGS; i++) {
        ir_get_recording_info(i, &s_playback_list[i]);
    }
}

static void draw_menu(void)
{
    oled_clear();
    oled_draw_text_center(0, "IR MONITOR", false);

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

    /* Header */
    char hdr[40];
    if (s_paused) {
        snprintf(hdr, sizeof(hdr), "RAW PAUSE");
    } else {
        snprintf(hdr, sizeof(hdr), "RAW LIVE #%04lu", (unsigned long)s_disp.seq);
    }
    oled_draw_text(0, 0, hdr, false);

    /* Save message */
    if (s_just_saved) {
        oled_draw_text_center(24, "SAVED! OK", false);
    } else if (!s_disp.valid) {
        oled_draw_text_center(24, "WAITING FOR IR...", false);
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

        if (s_disp.nec_ok) {
            snprintf(line, sizeof(line), "HEX %08lX", (unsigned long)s_disp.nec_raw);
        } else {
            snprintf(line, sizeof(line), "HINT %s", raw_hint());
        }
        oled_draw_text(0, 40, line, false);
    }

    /* Footer */
    if (s_paused) {
        oled_draw_text_center(56, "OK:Resume UP:Save BK", false);
    } else {
        oled_draw_text_center(56, "OK:Pause BK:Back", false);
    }
    oled_flush();
}

static void draw_nec(void)
{
    oled_clear();

    /* Header */
    char hdr[40];
    if (s_paused) {
        snprintf(hdr, sizeof(hdr), "NEC PAUSE");
    } else {
        snprintf(hdr, sizeof(hdr), "NEC LIVE #%04lu", (unsigned long)s_disp.seq);
    }
    oled_draw_text(0, 0, hdr, false);

    /* Save message */
    if (s_just_saved) {
        oled_draw_text_center(24, "SAVED! OK", false);
    } else if (!s_disp.valid) {
        oled_draw_text_center(24, "WAITING FOR NEC...", false);
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
    }

    /* Footer */
    if (s_paused) {
        oled_draw_text_center(56, "OK:Resume UP:Save BK", false);
    } else {
        oled_draw_text_center(56, "OK:Pause BK:Back", false);
    }
    oled_flush();
}

static void draw_playback(void)
{
    oled_clear();
    oled_draw_text_center(0, "PLAYBACK", false);

    s_playback_count = ir_get_saved_recording_count();
    if (s_playback_count == 0) {
        oled_draw_text_center(24, "NO RECORDINGS", false);
        oled_draw_text_center(40, "RECORD FIRST", false);
    } else {
        char line[40];
        snprintf(line, sizeof(line), "%lu/%lu", (unsigned long)(s_playback_sel + 1), (unsigned long)s_playback_count);
        oled_draw_text(0, 8, line, false);

        /* Calculate scroll window - selection moves first, then page scrolls */
        int total = (int)s_playback_count;
        int sel = s_playback_sel;
        if (total > MAX_VISIBLE_ITEMS) {
            /* Clamp start so sel is visible */
            if (sel < s_pb_start) s_pb_start = sel;
            if (sel >= s_pb_start + MAX_VISIBLE_ITEMS) s_pb_start = sel - MAX_VISIBLE_ITEMS + 1;
            if (s_pb_start < 0) s_pb_start = 0;
            if (s_pb_start > total - MAX_VISIBLE_ITEMS) s_pb_start = total - MAX_VISIBLE_ITEMS;
        } else {
            s_pb_start = 0;
        }
        int start = s_pb_start;
        int visible = total - start;
        if (visible > MAX_VISIBLE_ITEMS) visible = MAX_VISIBLE_ITEMS;

        /* Show recordings */
        for (int i = 0; i < visible; i++) {
            int idx = start + i;
            int y = 18 + i * 8;
            bool is_sel = (idx == sel);

            if (is_sel) {
                oled_fill_rect(0, y, OLED_W - 1 - 6, y + FONT5X7_CH_H, true);
            }

            if (idx < IR_MAX_RECORDINGS && s_playback_list[idx].valid) {
                snprintf(line, sizeof(line), "#%02lu %s %lu.%02lums",
                         (unsigned long)s_playback_list[idx].index,
                         ir_protocol_str(s_playback_list[idx].protocol),
                         (unsigned long)(s_playback_list[idx].total_duration_us / 1000),
                         (unsigned long)((s_playback_list[idx].total_duration_us % 1000) / 10));
                oled_draw_text(2, y, line, is_sel);
            }
        }

        /* Draw scrollbar on right side */
        if (total > MAX_VISIBLE_ITEMS) {
            int bar_x = OLED_W - 4;
            int bar_y = 18;
            int bar_h = MAX_VISIBLE_ITEMS * 8 - 1;
            /* Draw scrollbar background */
            oled_fill_rect(bar_x, bar_y, bar_x + 3, bar_y + bar_h, false);
            /* Draw scrollbar thumb */
            int thumb_h = (bar_h * visible) / total;
            if (thumb_h < 4) thumb_h = 4;
            int scroll_range = total - visible;
            int thumb_y = (scroll_range > 0) ? bar_y + (bar_h - thumb_h) * start / scroll_range : bar_y;
            oled_fill_rect(bar_x, thumb_y, bar_x + 3, thumb_y + thumb_h, true);
        }
    }

    if (s_playback_playing) {
        oled_draw_text_center(56, "PLAYING...", false);
    } else {
        oled_draw_text_center(56, "OK:Play x3  BK:Back", false);
    }
    oled_flush();
}

static void draw_storage(void)
{
    oled_clear();
    oled_draw_text_center(0, "STORAGE MANAGER", false);

    uint32_t total = 0, used = 0;
    ir_get_storage_info(&total, &used);

    char line[40];
    snprintf(line, sizeof(line), "USED: %luKB", (unsigned long)(used / 1024));
    oled_draw_text(0, 12, line, false);
    snprintf(line, sizeof(line), "FREE: %luKB", (unsigned long)((total - used) / 1024));
    oled_draw_text(0, 20, line, false);
    snprintf(line, sizeof(line), "RECORDS: %lu", (unsigned long)ir_get_saved_recording_count());
    oled_draw_text(0, 28, line, false);

    /* Menu items */
    static const char *items[] = {"DELETE ALL", "DELETE ONE"};
    for (int i = 0; i < 2; i++) {
        int y = 40 + i * 8;
        bool sel = (i == s_storage_sel);
        if (sel) {
            oled_fill_rect(0, y, OLED_W - 1, y + FONT5X7_CH_H, true);
        }
        oled_draw_text(6, y, items[i], sel);
    }

    oled_draw_text_center(56, "OK:Select BK:Back", false);
    oled_flush();
}

static void draw_delete_confirm(void)
{
    oled_clear();
    oled_draw_text_center(0, "DELETE ALL?", false);
    oled_draw_text_center(20, "THIS CANNOT BE", false);
    oled_draw_text_center(28, "UNDONE!", false);
    oled_draw_text_center(44, "OK:Confirm BK:Cancel", false);
    oled_flush();
}

static void draw_delete_select(void)
{
    oled_clear();
    oled_draw_text_center(0, "SELECT TO DELETE", false);

    /* Use cached count from load_playback_list() for consistency */
    if (s_playback_count == 0) {
        oled_draw_text_center(24, "NO RECORDINGS", false);
    } else {
        char line[40];
        snprintf(line, sizeof(line), "%lu/%lu", (unsigned long)(s_delete_sel + 1), (unsigned long)s_playback_count);
        oled_draw_text(0, 10, line, false);

        /* Calculate scroll window - selection moves first, then page scrolls */
        int total = (int)s_playback_count;
        int sel = s_delete_sel;
        if (total > MAX_VISIBLE_ITEMS) {
            /* Clamp start so sel is visible */
            if (sel < s_del_start) s_del_start = sel;
            if (sel >= s_del_start + MAX_VISIBLE_ITEMS) s_del_start = sel - MAX_VISIBLE_ITEMS + 1;
            if (s_del_start < 0) s_del_start = 0;
            if (s_del_start > total - MAX_VISIBLE_ITEMS) s_del_start = total - MAX_VISIBLE_ITEMS;
        } else {
            s_del_start = 0;
        }
        int start = s_del_start;
        int visible = total - start;
        if (visible > MAX_VISIBLE_ITEMS) visible = MAX_VISIBLE_ITEMS;

        /* Show recordings */
        for (int i = 0; i < visible; i++) {
            int idx = start + i;
            int y = 18 + i * 8;
            bool is_sel = (idx == sel);

            if (is_sel) {
                oled_fill_rect(0, y, OLED_W - 1 - 6, y + FONT5X7_CH_H, true);
            }

            if (idx < IR_MAX_RECORDINGS && s_playback_list[idx].valid) {
                snprintf(line, sizeof(line), "#%02lu %s %lu.%02lums",
                         (unsigned long)s_playback_list[idx].index,
                         ir_protocol_str(s_playback_list[idx].protocol),
                         (unsigned long)(s_playback_list[idx].total_duration_us / 1000),
                         (unsigned long)((s_playback_list[idx].total_duration_us % 1000) / 10));
                oled_draw_text(2, y, line, is_sel);
            }
        }

        /* Draw scrollbar on right side */
        if (total > MAX_VISIBLE_ITEMS) {
            int bar_x = OLED_W - 4;
            int bar_y = 18;
            int bar_h = MAX_VISIBLE_ITEMS * 8 - 1;
            /* Draw scrollbar background */
            oled_fill_rect(bar_x, bar_y, bar_x + 3, bar_y + bar_h, false);
            /* Draw scrollbar thumb */
            int thumb_h = (bar_h * visible) / total;
            if (thumb_h < 4) thumb_h = 4;
            int scroll_range = total - visible;
            int thumb_y = (scroll_range > 0) ? bar_y + (bar_h - thumb_h) * start / scroll_range : bar_y;
            oled_fill_rect(bar_x, thumb_y, bar_x + 3, thumb_y + thumb_h, true);
        }
    }

    oled_draw_text_center(56, "OK:Delete BK:Back", false);
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
    case SCR_PLAYBACK:
        draw_playback();
        break;
    case SCR_STORAGE:
        draw_storage();
        break;
    case SCR_DELETE_CONFIRM:
        draw_delete_confirm();
        break;
    case SCR_DELETE_SELECT:
        draw_delete_select();
        break;
    }
}

static void handle_buttons(void)
{
    bool redraw = false;
    uint32_t now = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);

    /* Clear save message after 2 seconds */
    if (s_just_saved && (now - s_save_msg_time > 2000)) {
        s_just_saved = false;
        redraw = true;
    }

    /* Handle playback completion */
    if (s_playback_playing && !ir_is_playing()) {
        s_playback_repeat++;
        if (s_playback_repeat < PLAYBACK_REPEAT_COUNT) {
            /* Repeat playback, stop on failure */
            if (ir_playback_start(s_playback_list[s_playback_sel].index) != ESP_OK) {
                ESP_LOGE("ui", "Playback repeat failed");
                s_playback_playing = false;
            }
        } else {
            s_playback_playing = false;
        }
        redraw = true;
    }

    if (btn_pressed(&s_btn_up)) {
        if (s_screen == SCR_MENU) {
            s_menu_sel = (s_menu_sel + MENU_ITEMS - 1) % MENU_ITEMS;
            redraw = true;
        } else if ((s_screen == SCR_RAW || s_screen == SCR_NEC) && s_paused) {
            /* Save last received frame when paused */
            if (ir_save_last_frame() == ESP_OK) {
                s_just_saved = true;
                s_save_msg_time = now;
            }
            redraw = true;
        } else if (s_screen == SCR_PLAYBACK && !s_playback_playing && s_playback_count > 0) {
            s_playback_sel = (s_playback_sel + s_playback_count - 1) % s_playback_count;
            redraw = true;
        } else if (s_screen == SCR_STORAGE) {
            s_storage_sel = (s_storage_sel + 1) % 2;
            redraw = true;
        } else if (s_screen == SCR_DELETE_SELECT && s_playback_count > 0) {
            s_delete_sel = (s_delete_sel + s_playback_count - 1) % s_playback_count;
            redraw = true;
        }
    }
    if (btn_pressed(&s_btn_down)) {
        if (s_screen == SCR_MENU) {
            s_menu_sel = (s_menu_sel + 1) % MENU_ITEMS;
            redraw = true;
        } else if (s_screen == SCR_PLAYBACK && !s_playback_playing && s_playback_count > 0) {
            s_playback_sel = (s_playback_sel + 1) % s_playback_count;
            redraw = true;
        } else if (s_screen == SCR_STORAGE) {
            s_storage_sel = (s_storage_sel + 1) % 2;
            redraw = true;
        } else if (s_screen == SCR_DELETE_SELECT && s_playback_count > 0) {
            s_delete_sel = (s_delete_sel + 1) % s_playback_count;
            redraw = true;
        }
    }
    if (btn_pressed(&s_btn_ok)) {
        if (s_screen == SCR_MENU) {
            switch (s_menu_sel) {
                case 0:
                    s_screen = SCR_RAW;
                    s_paused = false;
                    break;
                case 1:
                    s_screen = SCR_NEC;
                    s_paused = false;
                    break;
                case 2:
                    s_screen = SCR_PLAYBACK;
                    s_playback_sel = 0;
                    s_pb_start = 0;
                    load_playback_list();
                    break;
                case 3:
                    s_screen = SCR_STORAGE;
                    s_storage_sel = 0;
                    break;
            }
            redraw = true;
        } else if (s_screen == SCR_RAW || s_screen == SCR_NEC) {
            /* Toggle pause/resume */
            s_paused = !s_paused;
            if (s_paused) {
                ir_freeze_last_frame();
            }
            redraw = true;
        } else if (s_screen == SCR_PLAYBACK) {
            if (!s_playback_playing && s_playback_count > 0 && s_playback_list[s_playback_sel].valid) {
                /* Start playback x3 */
                s_playback_playing = true;
                s_playback_repeat = 0;
                ir_playback_start(s_playback_list[s_playback_sel].index);
                redraw = true;
            }
        } else if (s_screen == SCR_STORAGE) {
            if (s_storage_sel == 0) {
                /* Delete all - show confirm */
                s_screen = SCR_DELETE_CONFIRM;
            } else {
                /* Delete one - show select */
                s_screen = SCR_DELETE_SELECT;
                s_delete_sel = 0;
                s_del_start = 0;
                load_playback_list();
            }
            redraw = true;
        } else if (s_screen == SCR_DELETE_CONFIRM) {
            /* Confirm delete all */
            ir_delete_all_recordings();
            s_screen = SCR_STORAGE;
            redraw = true;
        } else if (s_screen == SCR_DELETE_SELECT) {
            if (s_playback_count > 0 && s_delete_sel < (int)s_playback_count
                && s_playback_list[s_delete_sel].valid) {
                ir_delete_recording(s_playback_list[s_delete_sel].index);
                load_playback_list();
                /* Clamp selection to valid range after list shrinks */
                if (s_playback_count == 0) {
                    s_delete_sel = 0;
                } else if (s_delete_sel >= (int)s_playback_count) {
                    s_delete_sel = (int)s_playback_count - 1;
                }
            }
            redraw = true;
        }
    }
    if (btn_pressed(&s_btn_back)) {
        if (s_screen == SCR_PLAYBACK) {
            if (s_playback_playing) {
                ir_playback_stop();
                s_playback_playing = false;
            }
        }
        if (s_screen != SCR_MENU) {
            s_screen = SCR_MENU;
            s_paused = false;
            s_delete_sel = 0;  /* Reset delete selection when leaving */
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

        /* Update IR display */
        if ((s_screen == SCR_RAW || s_screen == SCR_NEC) && !s_paused) {
            ir_frame_t f;
            if (ir_get_frame(&f)) {
                s_disp = f;
                draw_screen();
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
