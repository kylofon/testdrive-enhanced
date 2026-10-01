#pragma once
/* The port's 8x8 text font, in place of the Kickstart topaz 8 (which is in the ROM, not on the game disk):
 * the FontStruction "Amiga Topaz" by Patrick H. Lauke, CC BY 3.0 (licenses/amiga-topaz/), rasterised to 8x8
 * cells by tools/gen_font8.py. Baseline row 6, descenders in row 7, 8-pixel advance. Characters 0x20-0x7E;
 * others draw a hollow box. */
#include "../types.h"

#define FONT8_W        8
#define FONT8_H        8
#define FONT8_BASELINE 6

extern const u8 font8_first, font8_last;
extern const u8 font8_glyphs[][8];

/* Row r (0 = top) of character c, bit 7 = leftmost pixel. */
u8 font8_row(u8 c, int r);
