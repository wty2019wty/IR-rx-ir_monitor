#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#define OLED_W 128
#define OLED_H 64

/**
 * Initialize I2C bus and SSD1315 (SSD1306-compatible) 128x64 OLED.
 */
esp_err_t oled_init(void);

/**
 * Framebuffer operations (128x64, 1-bit per pixel).
 */
void oled_clear(void);
void oled_flush(void);
void oled_pixel(int x, int y, bool on);
void oled_fill_rect(int x0, int y0, int x1, int y1, bool on);
void oled_invert_rect(int x0, int y0, int x1, int y1);

/**
 * 5x7 text output. `invert` draws the glyph inverted (white on black bar).
 */
void oled_draw_char(int x, int y, char c, bool invert);
void oled_draw_text(int x, int y, const char *s, bool invert);
void oled_draw_text_center(int y, const char *s, bool invert);
int oled_text_width(const char *s);
