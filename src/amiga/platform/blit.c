/* Shapes and the blitter — port of td 0x10BA8-0x10C5A (arena, masks), 0x10D14-0x11552 (blitter routines),
 * 0x14984 (draw_shape_on_view) and 0x15472/0x15490 (find_shape) (port/amiga/spec/platform_video.md §2.3, §4.6;
 * shapes and archives: FORMATS.md, Amiga section).
 *
 * The original programs the custom chips directly (A6 = $DFF000). The port keeps that: the routines write the
 * registers of a small blitter emulator (below), which runs the whole blit synchronously when BLTSIZE is written,
 * so the routines port one to one and every wait returns at once. The clipping and parameter code works on the
 * BlitParams block at D:0362 and on the clip window D:0380..D:0386 in amem[], with the 68000's 16-bit arithmetic
 * (including the flag semantics of its signed branches) kept where it matters.
 *
 * Shape header (FORMATS.md): +0 width bytes, +2 height, +4 hot x, +6 hot y, +8 x, +A y, +C planes, +E plane
 * bytes, then the planes. BlitParams D:0362: +0 source, +4 mask, +8 destination offset, +A BLTSIZE, +C C/D
 * modulo, +E B modulo (and A in the word path), +10 A modulo, +12 plane bytes, +14 left clipped, +16 right
 * clipped, +18 shift, +1A FWM, +1C LWM. */
#include "blit.h"

#include "platform.h"
#include "../asymbols.h"
#include "../agfx.h"

/* ================================================================ blitter emulator */

/* Custom-chip register offsets from $DFF000. */
enum {
    BLTCON0 = 0x040, BLTCON1 = 0x042, BLTAFWM = 0x044, BLTALWM = 0x046,
    BLTCPT = 0x048, BLTBPT = 0x04C, BLTAPT = 0x050, BLTDPT = 0x054, BLTSIZE = 0x058,
    BLTCMOD = 0x060, BLTBMOD = 0x062, BLTAMOD = 0x064, BLTDMOD = 0x066,
    BLTCDAT = 0x070, BLTBDAT = 0x072, BLTADAT = 0x074,
};

static struct {
    u16 con0, con1, afwm, alwm;
    APTR apt, bpt, cpt, dpt;
    u16 amod, bmod, cmod, dmod;
    u16 adat, bdat, cdat;
} blt;

/* D = the minterm (BLTCON0 bits 7..0) of A, B, C, bit by bit: bit 7 = ABC, bit 6 = ABc, ... bit 0 = abc. */
static u16 minterm(u8 mt, u16 a, u16 b, u16 c)
{
    u16 d = 0;
    if (mt & 0x80) d |= a & b & c;
    if (mt & 0x40) d |= a & b & (u16)~c;
    if (mt & 0x20) d |= a & (u16)~b & c;
    if (mt & 0x10) d |= a & (u16)~b & (u16)~c;
    if (mt & 0x08) d |= (u16)~a & b & c;
    if (mt & 0x04) d |= (u16)~a & b & (u16)~c;
    if (mt & 0x02) d |= (u16)~a & (u16)~b & c;
    if (mt & 0x01) d |= (u16)~a & (u16)~b & (u16)~c;
    return d;
}

/* One blit, ascending, area mode (the game never sets DESC, LINE or the fill bits). Per word: A (DMA or BLTADAT)
 * is masked with FWM on the first and LWM on the last word of each row (both when the row is one word), then
 * shifted right by ASH, taking the bits shifted in from the previous masked A word; B is shifted by BSH with the
 * previous B word. The previous words start at 0 with each blit and carry across rows, as on the real chip (and
 * in UAE). Modulos are added after each row to the pointers of the channels in use. */
static void blit_run(u16 size)
{
    u32 h = size >> 6, w = size & 0x3F;
    if (h == 0) h = 1024;
    if (w == 0) w = 64;
    bool use_a = blt.con0 & 0x0800, use_b = blt.con0 & 0x0400, use_c = blt.con0 & 0x0200, use_d = blt.con0 & 0x0100;
    u8 mt = (u8)blt.con0;
    unsigned ash = blt.con0 >> 12, bsh = blt.con1 >> 12;
    u16 preva = 0, prevb = 0, bhold = blt.bdat;
    for (u32 r = 0; r < h; r++) {
        for (u32 c = 0; c < w; c++) {
            if (use_a) { blt.adat = rd16(blt.apt); blt.apt += 2; }
            u16 a = blt.adat;
            if (c == 0) a &= blt.afwm;
            if (c == w - 1) a &= blt.alwm;
            u16 ahold = (u16)(((u32)preva << 16 | a) >> ash);
            preva = a;
            if (use_b) {
                blt.bdat = rd16(blt.bpt); blt.bpt += 2;
                bhold = (u16)(((u32)prevb << 16 | blt.bdat) >> bsh);
                prevb = blt.bdat;
            }
            if (use_c) { blt.cdat = rd16(blt.cpt); blt.cpt += 2; }
            u16 d = minterm(mt, ahold, bhold, blt.cdat);
            if (use_d) { wr16(blt.dpt, d); blt.dpt += 2; }
        }
        if (use_a) blt.apt += (u32)(s32)(s16)blt.amod;
        if (use_b) blt.bpt += (u32)(s32)(s16)blt.bmod;
        if (use_c) blt.cpt += (u32)(s32)(s16)blt.cmod;
        if (use_d) blt.dpt += (u32)(s32)(s16)blt.dmod;
    }
}

/* move.w #v, reg(a6) */
static void custom_w16(u16 reg, u16 v)
{
    switch (reg) {
    case BLTCON0: blt.con0 = v; break;
    case BLTCON1: blt.con1 = v; break;
    case BLTAFWM: blt.afwm = v; break;
    case BLTALWM: blt.alwm = v; break;
    case BLTCMOD: blt.cmod = v & 0xFFFE; break;
    case BLTBMOD: blt.bmod = v & 0xFFFE; break;
    case BLTAMOD: blt.amod = v & 0xFFFE; break;
    case BLTDMOD: blt.dmod = v & 0xFFFE; break;
    case BLTCDAT: blt.cdat = v; break;
    case BLTBDAT: blt.bdat = v; break;
    case BLTADAT: blt.adat = v; break;
    case BLTSIZE: blit_run(v); break;
    default: break;
    }
}

/* move.l a, reg(a6): the pointer registers (word-aligned, as the hardware ignores bit 0). */
static void custom_w32(u16 reg, APTR a)
{
    a &= ~1u;
    switch (reg) {
    case BLTAPT: blt.apt = a; break;
    case BLTBPT: blt.bpt = a; break;
    case BLTCPT: blt.cpt = a; break;
    case BLTDPT: blt.dpt = a; break;
    default: break;
    }
}

/* ================================================================ 68000 helpers */

/* `ble` after add.w/sub.w: Z or N^V, i.e. the wrapped 16-bit result is 0 or the exact result is negative. */
static bool le16(s32 exact) { return (s16)exact == 0 || exact < 0; }

#define P            DADDR(D_blit_params)
#define P_SRC        0x00
#define P_MSK        0x04
#define P_DOFF       0x08
#define P_SIZE       0x0A
#define P_DMOD       0x0C
#define P_BMOD       0x0E
#define P_AMOD       0x10
#define P_PBYTES     0x12
#define P_LFLAG      0x14
#define P_RFLAG      0x16
#define P_SHIFT      0x18
#define P_FWM        0x1A
#define P_LWM        0x1C

static u16  pw(u32 o) { return rd16(P + o); }
static void setpw(u32 o, u32 v) { wr16(P + o, (u16)v); }
static u32  pl(u32 o) { return rd32(P + o); }
static void setpl(u32 o, u32 v) { wr32(P + o, v); }

static APTR dest_plane(int p) { return D32(D_blit_dest + 4 * p); }

/* ================================================================ blitter routines */

static void blit_wait_hw(void)                           /* 0x10D44: the emulated blitter is never busy */
{
    SETD32(D_blit_wait_calls, D32(D_blit_wait_calls) + 1);
}

void blit_wait(void) { blit_wait_hw(); }                 /* 0x10E78 */

void gfx_OwnBlitter(void) {}
void gfx_DisownBlitter(void) {}

void blit_begin(void)                                    /* 0x10D14 */
{
    blit_wait_hw();
    custom_w16(BLTAMOD, 0);
    custom_w16(BLTDMOD, 0);
    custom_w16(BLTCON0, 0x09F0);
    custom_w16(BLTCON1, 0);
    custom_w16(BLTAFWM, 0xFFFF);
    custom_w16(BLTALWM, 0xFFFF);
}

/* 0x10D72 / 0x10DBE: a word-aligned rectangle of one plane, cleared (BLTCON0 0x0100) or set (0x01FF). The
 * row offset is y*8 + y*32 with two `adda.w`, the byte offset x `lsr.w` 3 and even. BLTCON1 is left as it is. */
static void blit_rect(APTR plane, s16 x, s16 y, s16 wbytes, s16 h, u16 con0)
{
    APTR a = plane;
    a += (u32)(s32)(s16)(u16)((u16)y << 3);
    a += (u32)(s32)(s16)(u16)((u16)y << 5);
    a += (u32)(s32)(s16)(u16)(((u16)x >> 3) & 0xFFFE);
    u16 dmod = (u16)(0x28 - wbytes);
    u16 size = (u16)((u16)h << 6 | (u16)wbytes >> 1);
    blit_wait_hw();
    custom_w16(BLTAFWM, 0xFFFF);
    custom_w16(BLTALWM, 0xFFFF);
    custom_w32(BLTDPT, a);
    custom_w16(BLTCON0, con0);
    custom_w16(BLTDMOD, dmod);
    custom_w16(BLTSIZE, size);
    blit_wait_hw();
}

void blit_clear_rect(APTR plane, s16 x, s16 y, s16 wbytes, s16 h) { blit_rect(plane, x, y, wbytes, h, 0x0100); }
void blit_fill_rect(APTR plane, s16 x, s16 y, s16 wbytes, s16 h)  { blit_rect(plane, x, y, wbytes, h, 0x01FF); }

void blit_set_dest(APTR planes5)                         /* 0x10E26 */
{
    for (u32 i = 0; i < 5; i++) SETD32(D_blit_dest + 4 * i, rd32(planes5 + 4 * i));
}

void set_clip(s16 top, s16 bottom, s16 left, s16 right)  /* 0x10FBC */
{
    SETD16(D_clip_top, top);
    SETD16(D_clip_bottom, bottom);
    SETD16(D_clip_left, (u16)left & 0xFFF0);
    SETD16(D_clip_right, (u16)right & 0xFFF0);
}

void set_clip_full(void) { set_clip(0, 0xC8, 0, 0x140); } /* 0x10FA4 */

/* The y clipping shared by both preps (0x11006-0x11036 / 0x1130A-0x1133A). Returns false when nothing is left;
 * *y and *h are updated, the source and mask pointers advanced past the rows cut at the top. */
static bool prep_clip_y(APTR s, s16 *y, s16 *h)
{
    s16 top = DS16(D_clip_top), bottom = DS16(D_clip_bottom);
    if (*y < top) {
        s16 d = (s16)(*y - top);                        /* sub.w */
        s32 ex = (s32)*h + d;                           /* add.w d1, d2; ble */
        *h = (s16)ex;
        if (le16(ex)) return false;
        u32 skip = (u32)(u16)(-d) * rd16(s);            /* neg.w; mulu.w width */
        setpl(P_SRC, pl(P_SRC) + skip);
        setpl(P_MSK, pl(P_MSK) + skip);
        *y = top;
    }
    s16 end = (s16)(*y + *h);                           /* add.w */
    s32 t = (s32)end - bottom;                          /* sub.w; ble */
    if (!le16(t)) {
        s32 ex = (s32)*h - (s16)t;
        *h = (s16)ex;
        if (le16(ex)) return false;
    }
    return true;
}

/* The destination offset: (x asr 3, even) + y*8 + y*32, all 16-bit. */
static u16 dest_offset(s16 x, s16 y)
{
    u16 d = (u16)(((u16)(x >> 3)) & 0xFFFE);
    d = (u16)(d + (u16)((u16)y << 3));
    d = (u16)(d + (u16)((u16)y << 5));
    return d;
}

/* 0x10FDA: clip and word-aligned parameters (x rounded down to 16). false = the carry set = nothing to draw. */
static bool blit_prep_word(APTR s, APTR m, s16 x, s16 y)
{
    if ((s32)s <= 0x32) return false;
    setpl(P_SRC, s + 0x10);
    setpl(P_MSK, m + 0x10);                             /* even for a NULL mask */
    setpl(P_LFLAG, 0);                                  /* clr.l: left and right flags */
    setpw(P_FWM, 0xFFFF);
    setpw(P_LWM, 0xFFFF);
    s16 h = (s16)rd16(s + 2);
    if (!prep_clip_y(s, &y, &h)) return false;
    u16 size = (u16)((u16)h << 6);
    setpw(P_BMOD, 0);
    x = (s16)((u16)x & 0xFFF0);
    s16 w = (s16)rd16(s);
    s16 left = DS16(D_clip_left), right = DS16(D_clip_right);
    if (x < left) {
        s16 d = (s16)((s16)(x - left) >> 3);             /* asr.w: negative */
        s32 ex = (s32)w + d;
        w = (s16)ex;
        if (le16(ex)) return false;
        setpw(P_BMOD, pw(P_BMOD) - (u16)d);
        setpl(P_SRC, pl(P_SRC) - (u32)(s32)d);
        setpl(P_MSK, pl(P_MSK) - (u32)(s32)d);
        x = left;
        setpw(P_LFLAG, 0xFFFF);
    }
    s16 t0 = (s16)((u16)((u16)w << 3) + (u16)x);        /* lsl.w; add.w */
    s32 t = (s32)t0 - right;                            /* sub.w; ble */
    if (!le16(t)) {
        s16 d = (s16)((s16)t >> 3);
        s32 ex = (s32)w - d;
        w = (s16)ex;
        if (le16(ex)) return false;
        setpw(P_BMOD, pw(P_BMOD) + (u16)d);
        setpw(P_RFLAG, 0xFFFF);
    }
    setpw(P_DMOD, 0x28 - w);
    size |= (u16)w >> 1;
    setpw(P_SIZE, size);
    setpw(P_DOFF, dest_offset(x, y));
    setpw(P_PBYTES, rd16(s + 0xE));
    return true;
}

/* 0x112E4: clip and shifted parameters: one extra source word per row, A and B modulo -2, shift = x & 15, FWM
 * masking the pixels cut at the left, LWM 0 (the extra word) or the pixels that stay at a right clip. */
static bool blit_prep_shift(APTR s, APTR m, s16 x, s16 y)
{
    if ((s32)s <= 0x32) return false;
    APTR a1 = m + 0x10;
    setpl(P_SRC, s + 0x10);
    setpl(P_MSK, a1);
    setpl(P_LFLAG, 0);
    setpw(P_PBYTES, rd16(s + 0xE));
    s16 h = (s16)rd16(s + 2);
    if (!prep_clip_y(s, &y, &h)) return false;
    u16 size = (u16)((u16)h << 6);
    setpw(P_FWM, 0xFFFF);
    setpw(P_LWM, 0);
    setpw(P_BMOD, 0xFFFE);
    setpw(P_AMOD, 0xFFFE);
    setpw(P_SHIFT, (u16)x & 0xF);
    s16 w = (s16)(rd16(s) + 2);
    s16 left = DS16(D_clip_left), right = DS16(D_clip_right);
    if (x < left) {
        s16 d4 = (s16)(x - left);                       /* negative */
        s16 d0 = (s16)((s16)(d4 + 0xF) >> 3);
        d0 = (s16)((u16)d0 & 0xFFFE);                   /* and.b #$FE: the flags test the low byte */
        if ((d0 & 0xFF) != 0) {
            s32 ex = (s32)w + d0;
            w = (s16)ex;
            if (le16(ex)) return false;
            setpw(P_BMOD, pw(P_BMOD) - (u16)d0);
            setpw(P_AMOD, pw(P_AMOD) - (u16)d0);
            setpl(P_SRC, pl(P_SRC) - (u32)(s32)d0);
            setpl(P_MSK, pl(P_MSK) - (u32)(s32)d0);
        }
        setpw(P_LFLAG, 0xFFFF);
        setpw(P_FWM, rd16(DADDR(D_fwm_table) + 2u * ((u16)-d4 & 0xF)));
        x = left;
        if (pw(P_SHIFT)) x = (s16)(x - 0x10);
    }
    x = (s16)((u16)x & 0xFFF0);
    s16 t0 = (s16)((u16)((u16)w << 3) + (u16)x);
    s32 t = (s32)t0 - right;
    if (!le16(t)) {
        s16 d = (s16)((s16)t >> 3);
        s32 ex = (s32)w - d;
        w = (s16)ex;
        if (le16(ex)) return false;
        setpw(P_BMOD, pw(P_BMOD) + (u16)d);
        setpw(P_AMOD, pw(P_AMOD) + (u16)d);
        setpw(P_RFLAG, 0xFFFF);
        setpw(P_LWM, rd16(DADDR(D_lwm_table) + 2u * (u16)(0x10 - pw(P_SHIFT))));
    }
    setpw(P_DMOD, 0x28 - w);
    size |= (u16)w >> 1;
    setpw(P_SIZE, size);
    setpw(P_DOFF, dest_offset(x, y));
    if ((s32)a1 < 0x7D0) setpl(P_MSK, 0);               /* no mask */
    return true;
}

/* The per-plane loop of all four shape blits: B (or A for the XOR-word blit) = shape plane p, C = D = the
 * destination plane D:0BEC[p] + offset; the source advances by `adda.w` plane bytes. */
static void blit_planes(u16 src_reg, APTR a_mask)
{
    APTR src = pl(P_SRC);
    u16 pbytes = pw(P_PBYTES), size = pw(P_SIZE);
    s16 doff = (s16)pw(P_DOFF);
    for (int p = 0; p < 5; p++) {
        if (p) blit_wait_hw();
        APTR d = dest_plane(p) + (u32)(s32)doff;
        if (a_mask) custom_w32(BLTAPT, a_mask);
        custom_w32(src_reg, src);
        custom_w32(BLTCPT, d);
        custom_w32(BLTDPT, d);
        custom_w16(BLTSIZE, size);
        src += (u32)(s32)(s16)pbytes;
    }
    blit_wait_hw();
}

/* 0x10E88: word-aligned cookie-cut 0x0FCA with a mask, plain copy 0x05CC (D = B) without. "No mask" is a mask
 * pointer below 0x7D0 after the clipping moved it (a NULL mask cut by many rows at the top reads as a mask from
 * low memory, as in the original). */
static void blit_shape_word_asm(APTR s, APTR m, s16 x, s16 y)
{
    if (!blit_prep_word(s, m, x, y)) return;
    APTR msk = pl(P_MSK);
    blit_wait_hw();
    custom_w16(BLTAFWM, 0xFFFF);
    custom_w16(BLTALWM, 0xFFFF);
    custom_w16(BLTAMOD, pw(P_BMOD));
    custom_w16(BLTBMOD, pw(P_BMOD));
    custom_w16(BLTCMOD, pw(P_DMOD));
    custom_w16(BLTDMOD, pw(P_DMOD));
    custom_w16(BLTCON1, 0);
    custom_w16(BLTCON0, (s32)msk >= 0x7D0 ? 0x0FCA : 0x05CC);
    custom_w32(BLTAPT, msk);                            /* written in both modes (0x05CC does not use A) */
    blit_planes(BLTBPT, msk);
}

/* 0x110C4: shifted cookie-cut 0x0FCA (ASH = BSH = x & 15), or masked copy 0x07CA with A = BLTADAT = 0xFFFF. */
static void blit_shape_asm(APTR s, APTR m, s16 x, s16 y)
{
    if (!blit_prep_shift(s, m, x, y)) return;
    APTR msk = pl(P_MSK);
    u16 sh = (u16)(pw(P_SHIFT) << 12);                  /* ror.w #4 */
    blit_wait_hw();
    custom_w16(BLTAFWM, pw(P_FWM));
    custom_w16(BLTALWM, pw(P_LWM));
    custom_w16(BLTCON1, sh);
    u16 con0 = 0x0FCA;
    if ((s32)msk < 0x7D0) { con0 = 0x07CA; custom_w16(BLTADAT, 0xFFFF); }
    custom_w16(BLTCON0, sh | con0);
    custom_w16(BLTAMOD, pw(P_AMOD));
    custom_w16(BLTBMOD, pw(P_BMOD));
    custom_w16(BLTCMOD, pw(P_DMOD));
    custom_w16(BLTDMOD, pw(P_DMOD));
    custom_w32(BLTAPT, msk);
    blit_planes(BLTBPT, msk);
}

void blit_shape_word(APTR s, APTR mask, s16 x, s16 y) { blit_shape_word_asm(s, mask, x, y); }   /* 0x10E3A */
void blit_shape(APTR s, APTR mask, s16 x, s16 y)      { blit_shape_asm(s, mask, x, y); }        /* 0x10E58 */

/* 0x111E6: shifted XOR, BLTCON0 = sh | 0x076A (B C D; D = C ^ A·B) with A = BLTADAT = 0xFFFF inside the edge
 * masks: the shape planes are XORed into the destination. */
void blit_shape_xor(APTR s, s16 x, s16 y)
{
    if (!blit_prep_shift(s, 0, x, y)) return;           /* a1 is passed through; its only use is the P+4 slot */
    u16 sh = (u16)(pw(P_SHIFT) << 12);
    blit_wait_hw();
    custom_w16(BLTAFWM, pw(P_FWM));
    custom_w16(BLTALWM, pw(P_LWM));
    custom_w16(BLTCON1, sh);
    custom_w16(BLTADAT, 0xFFFF);
    custom_w16(BLTCON0, sh | 0x076A);
    custom_w16(BLTAMOD, pw(P_AMOD));
    custom_w16(BLTBMOD, pw(P_BMOD));
    custom_w16(BLTCMOD, pw(P_DMOD));
    custom_w16(BLTDMOD, pw(P_DMOD));
    blit_planes(BLTBPT, 0);
}

/* 0x11432: word-aligned XOR (0x0B5A: A C D, D = A ^ C). The right clip is rounded up to 16 for the prep and the
 * last word masked to the true clip instead. */
void blit_shape_xor_word(APTR s, s16 x, s16 y)
{
    u16 saved = D16(D_clip_right);
    SETD16(D_clip_right, (u16)(saved + 0xF) & 0xFFF0);
    if (blit_prep_word(s, 0, x, y)) {
        s16 end = (s16)((u16)x + (u16)(rd16(s) << 3));  /* the unrounded x */
        if (end > (s16)saved) {
            u16 off = (u16)((((saved & 0xF) << 1) - 2) & 0x1F);
            setpw(P_LWM, rd16(DADDR(D_lwm_table) + 2 + off));
        }
        blit_wait_hw();
        custom_w16(BLTAFWM, 0xFFFF);
        custom_w16(BLTALWM, pw(P_LWM));
        custom_w16(BLTAMOD, pw(P_BMOD));
        custom_w16(BLTCMOD, pw(P_DMOD));
        custom_w16(BLTDMOD, pw(P_DMOD));
        custom_w16(BLTCON1, 0);
        custom_w16(BLTCON0, 0x0B5A);
        blit_planes(BLTAPT, 0);
    }
    SETD16(D_clip_right, saved);
}

/* ================================================================ arena, masks, archives */

APTR arena_new(s32 size)                                 /* 0x10BA8 */
{
    APTR p = alloc_chip((u32)size);
    if (p) {
        wr32(p, (u32)size);
        wr32(p + 4, 8);
    }
    return p;
}

APTR arena_alloc(APTR a, s32 n)                          /* 0x10BDC */
{
    APTR r = 0;
    if (!a) return 0;
    if ((s32)(rd32(a) - rd32(a + 4)) >= n) {
        r = a + rd32(a + 4);
        wr32(a + 4, rd32(a + 4) + (u32)n);
    }
    SETD16(D_arena_flag, 0);                            /* cleared on failure too */
    return r;
}

s32 mask_size(APTR s)                                    /* 0x10C3E */
{
    return (s32)((u32)rd16(s) * rd16(s + 2) + 0x10);
}

/* 0x10C5A: a 1-plane shape, the OR of all planes (colour 0 = transparent), header copied with planes = 1. */
APTR make_mask(APTR arena, APTR s)
{
    if (!s) return 0;
    APTR m = arena_alloc(arena, mask_size(s));
    if (!m) return 0;
    for (u32 i = 0; i < 16; i += 4) wr32(m + i, rd32(s + i));
    wr16(m + 0xC, 1);
    u16 pb = rd16(s + 0xE);
    s16 words = (s16)(pb >> 1);
    s16 planes = (s16)rd16(s + 0xC);
    if (words <= 0 || planes <= 0) return m;
    APTR a2 = s + 0x10, a3 = m + 0x10;
    for (s16 i = 0; i < words; i++, a2 += 2, a3 += 2) {
        u16 d3 = 0, v = 0;
        for (s16 p = 0; p < planes; p++, d3 = (u16)(d3 + pb)) v |= rd16(a2 + (u32)(s32)(s16)d3);
        wr16(a3, v);
    }
    return m;
}

/* 0x15490: +4 count (word), +6 names (longs, sorted: the search stops at the first name >= the one wanted, as
 * signed longs), then the offsets (longs, from the data at +6 + 8 * count). 0 when missing. */
APTR find_shape(APTR a, u32 name4)
{
    s16 n = (s16)rd16(a + 4);
    if (n <= 0) return 0;
    APTR p = a + 6;
    s16 d1 = n;
    for (;;) {                                          /* cmp.l (a1)+, d0; dble d1 */
        s32 e = (s32)rd32(p);
        p += 4;
        if ((s32)name4 <= e) break;
        if (--d1 == -1) break;
    }
    p -= 4;
    s16 idx = (s16)((p - a - 6) >> 2);
    if (rd32(p) != name4) return 0;
    return rd32(a + 6 + (u32)((s32)n * 4) + (u32)((s32)idx * 4)) + a + (u32)((s32)n * 8) + 6;
}

void draw_shape_on_view(APTR s, APTR view)               /* 0x14984 */
{
    set_clip_full();
    gfx_OwnBlitter();
    APTR bm = rd32(rd32(rd32(view + VIEW_ViewPort) + VP_RasInfo) + RI_BitMap);
    blit_set_dest(bm + BM_Planes);
    blit_shape_word(s, 0, (s16)rd16(s + 8), (s16)rd16(s + 0xA));
    blit_wait();
    gfx_DisownBlitter();
}
