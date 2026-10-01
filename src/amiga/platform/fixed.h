#pragma once
/* Fixed-point and math helpers of the root hunk, used by the drive — port of td 0x12056-0x12160 and the Aztec
 * runtime division (port/amiga/spec/platform_video.md §2.8, §4.15). */
#include "../amem.h"

s16 abs_w(s16 v);                       /* 0x12056 */
s32 abs_l(s32 v);                       /* 0x1205E */
s32 sin_deg(s32 deg_16_16);             /* 0x12108: interpolated sine from the table at 0x1216C */
s32 cos_deg(s16 deg);                   /* 0x12146 */
s32 ldiv_68k(s32 a, s32 b);             /* 0x16ED8: signed long division as the Aztec runtime does it */
