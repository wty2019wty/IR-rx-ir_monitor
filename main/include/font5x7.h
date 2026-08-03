#pragma once

#include <stdint.h>
#include <stddef.h>

#define FONT5X7_CH_W       5
#define FONT5X7_CH_H       7
#define FONT5X7_CH_ADVANCE 6
#define FONT5X7_FIRST_CHAR 0x20
#define FONT5X7_LAST_CHAR  0x7E

/**
 * Return the 5-byte column bitmap for a character.
 * Unsupported characters fall back to space.
 */
const uint8_t *font5x7_glyph(char c);

/**
 * Pixel width of a string with 6px per-character advance.
 */
int font5x7_text_width(const char *s);
