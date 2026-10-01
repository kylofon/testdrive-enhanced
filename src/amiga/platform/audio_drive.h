#pragma once
/* The drive's sounds — port of td 0x26B7A-0x26EE2 (port/amiga/spec/platform_audio.md §4.13, §4.14, §4.17):
 * the five drive samples, the engine/turbo/squeal loops and the bump and radar one-shots. README bugs 8
 * (sound off at stage start) and 9 (the loops include the 6-byte header) are fixed here by default. */
#include "../amem.h"

void sfx_load_drive(void);               /* 0x26B7A */
void sfx_free_drive(void);               /* 0x26C02 */
void radar_beep(void);                   /* 0x26C50 */
void engine_sound_start(void);           /* 0x26C70 */
void engine_sound_fade_out(void);        /* 0x26D16 */
void engine_sound_update(void);          /* 0x26D4E: once per drive frame */
