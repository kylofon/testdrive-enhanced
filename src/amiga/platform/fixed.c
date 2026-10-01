/* Fixed-point and math helpers of the root hunk — port of td 0x12056-0x12168 and the Aztec runtime's signed
 * long division 0x16ED8 (port/amiga/spec/platform_video.md §2.8, §4.15).
 *
 * The sine table is data of the code hunk at 0x1216C (91 words, round(sin(i deg) * 65535) for i = 0..90), so it
 * is read straight out of the loaded image instead of being duplicated here. */
#include "fixed.h"

#define SIN_TABLE 0x1216Cu       /* 0x1216C: sin(i deg) * 65535, i = 0..90 (platform_video §4.15) */

s16 abs_w(s16 v)                                         /* 0x12056: tst.w / neg.w */
{
    return v < 0 ? (s16)(-(s32)v) : v;                   /* neg.w of -0x8000 gives -0x8000 again */
}

s32 abs_l(s32 v)                                         /* 0x1205E: tst.l / neg.l */
{
    return v < 0 ? (s32)(0u - (u32)v) : v;
}

/* 0x12108: sine of a 16.16 angle. The integer part is taken as a byte: above 90 it is folded with 0xB4 - i
 * (180 - i), then masked to 7 bits, so only 0..180 degrees are meaningful. The fraction interpolates linearly
 * between the two table entries (mulu.w of the unsigned fraction by the unsigned difference, >> 16). */
s32 sin_deg(s32 deg_16_16)
{
    u16 frac = (u16)deg_16_16;
    u8 i = (u8)((u32)deg_16_16 >> 16);                   /* swap d0; the byte compare below is signed */
    if ((s8)i > 0x5A) i = (u8)(0xB4 - i);                /* sub.b #$b4,d0; neg.b d0 */
    i &= 0x7F;                                           /* and.w #$7f,d0 */
    u16 lo = rd16(SIN_TABLE + (u32)i * 2);
    u16 hi = rd16(SIN_TABLE + (u32)i * 2 + 2);
    return (s32)((u32)lo + (((u32)frac * (u32)(u16)(hi - lo)) >> 16));
}

/* 0x12146: cosine of a whole number of degrees, table[|90 - d|] with a 7-bit mask (valid for -90..90). */
s32 cos_deg(s16 deg)
{
    u16 i = (u16)((deg >= 0 ? (s16)(0x5A - deg) : (s16)(deg + 0x5A)) & 0x7F);
    return (s32)(u32)rd16(SIN_TABLE + (u32)i * 2);
}

/* 0x16ED8 (_divs32, via the unsigned core 0x16F30): sign-magnitude, so the division truncates toward zero.
 * The core divides by zero on a zero divisor (a 68000 trap); the port returns 0 instead, which no caller
 * reaches. */
s32 ldiv_68k(s32 a, s32 b)
{
    if (b == 0) return 0;
    u32 ua = (a < 0) ? 0u - (u32)a : (u32)a;             /* neg.l; 0x80000000 stays 0x80000000 */
    u32 ub = (b < 0) ? 0u - (u32)b : (u32)b;
    u32 q = ua / ub;
    return ((a < 0) != (b < 0)) ? (s32)(0u - q) : (s32)q;
}
