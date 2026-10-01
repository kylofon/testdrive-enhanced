/* graphics.library RastPort drawing, as far as the game uses it: pens and draw modes, Move/Draw, RectFill,
 * Text/TextLength with the port's topaz-8-style font (font8.h), ScrollRaster, and OpenFont/SetFont/CloseFont
 * (port/amiga/spec/platform_video.md §4.7, game_flow.md, title_select.md).
 *
 * Everything draws into the RastPort's BitMap (RP_BitMap: BytesPerRow, Rows, Depth, Planes) in amem[], writing
 * only the planes enabled in RP_Mask. The game's RastPorts have no Layer, so graphics.library does no clipping;
 * the port clips to the BitMap so that a stray coordinate cannot write outside it.
 *
 * Draw modes: JAM1 draws the 1 bits of the pattern (glyph, line pattern, solid fill) in FgPen and leaves the 0
 * bits; JAM2 draws the 0 bits in BgPen too; COMPLEMENT inverts the destination (every enabled plane) where the
 * pattern is 1; INVERSVID inverts the pattern first. */
#include "platform/blit.h"

#include "agfx.h"
#include "font8.h"

#define JAM1       0
#define JAM2       1
#define COMPLEMENT 2
#define INVERSVID  4

/* struct RastPort fields beyond agfx.h */
#define RP_TxHeight   0x3A
#define RP_TxWidth    0x3C
#define RP_TxBaseline 0x3E
#define RP_TxSpacing  0x40

/* struct TextAttr / TextFont (1.3) */
#define TA_YSize      0x04
#define TF_YSize      0x14
#define TF_Style      0x16
#define TF_Flags      0x17
#define TF_XSize      0x18
#define TF_Baseline   0x1A
#define TF_BoldSmear  0x1C
#define TF_Accessors  0x1E
#define TF_LoChar     0x20
#define TF_HiChar     0x21
#define TF_SIZE       0x34
#define FPF_ROMFONT   0x01
#define FPF_DESIGNED  0x40

typedef struct {
    APTR planes[8];
    u16 bpr, rows, width;
    u8 depth, mask, fg, bg, mode;
} Pen;

static bool pen_setup(APTR rp, Pen *p)
{
    APTR bm = rd32(rp + RP_BitMap);
    if (!bm) return false;
    p->bpr = rd16(bm + BM_BytesPerRow);
    p->rows = rd16(bm + BM_Rows);
    p->width = (u16)(p->bpr * 8u);
    p->depth = rd8(bm + BM_Depth);
    if (p->depth > 8) p->depth = 8;
    for (u8 i = 0; i < p->depth; i++) p->planes[i] = rd32(bm + BM_Planes + 4u * i);
    p->mask = rd8(rp + RP_Mask);
    p->fg = rd8(rp + RP_FgPen);
    p->bg = rd8(rp + RP_BgPen);
    p->mode = rd8(rp + RP_DrawMode);
    return true;
}

/* One pixel of pattern bit `bit` in the RastPort's draw mode. */
static void pen_pixel(const Pen *p, s32 x, s32 y, bool bit)
{
    if (x < 0 || y < 0 || x >= p->width || y >= p->rows) return;
    if (p->mode & INVERSVID) bit = !bit;
    u8 m = (u8)(0x80 >> (x & 7));
    u32 off = (u32)y * p->bpr + (u32)(x >> 3);
    if (p->mode & COMPLEMENT) {
        if (!bit) return;
        for (u8 i = 0; i < p->depth; i++)
            if (p->mask & (1u << i)) { APTR a = p->planes[i] + off; wr8(a, rd8(a) ^ m); }
        return;
    }
    u8 pen;
    if (bit) pen = p->fg;
    else if (p->mode & JAM2) pen = p->bg;
    else return;
    for (u8 i = 0; i < p->depth; i++) {
        if (!(p->mask & (1u << i))) continue;
        APTR a = p->planes[i] + off;
        wr8(a, (pen >> i) & 1 ? rd8(a) | m : rd8(a) & (u8)~m);
    }
}

static bool pen_get(const Pen *p, u8 plane, s32 x, s32 y)
{
    return rd8(p->planes[plane] + (u32)y * p->bpr + (u32)(x >> 3)) & (0x80 >> (x & 7));
}

void gfx_SetAPen(APTR rp, u8 pen) { wr8(rp + RP_FgPen, pen); }
void gfx_SetBPen(APTR rp, u8 pen) { wr8(rp + RP_BgPen, pen); }
void gfx_SetDrMd(APTR rp, u8 mode) { wr8(rp + RP_DrawMode, mode); }

void gfx_Move(APTR rp, s16 x, s16 y)
{
    wr16(rp + RP_cp_x, (u16)x);
    wr16(rp + RP_cp_y, (u16)y);
}

/* A line from the current position to (x, y), both ends included, with the line pattern (LinePtrn, from its
 * top bit at the first pixel); the current position moves to (x, y). */
void gfx_Draw(APTR rp, s16 x, s16 y)
{
    s32 x0 = (s16)rd16(rp + RP_cp_x), y0 = (s16)rd16(rp + RP_cp_y), x1 = x, y1 = y;
    gfx_Move(rp, x, y);
    Pen p;
    if (!pen_setup(rp, &p)) return;
    u16 pat = rd16(rp + RP_LinePtrn);
    s32 dx = x1 > x0 ? x1 - x0 : x0 - x1, dy = y1 > y0 ? y1 - y0 : y0 - y1;
    s32 sx = x1 >= x0 ? 1 : -1, sy = y1 >= y0 ? 1 : -1;
    s32 n = dx > dy ? dx : dy;
    s32 err = (dx > dy ? dx : dy) / 2;
    for (s32 i = 0; i <= n; i++) {
        pen_pixel(&p, x0, y0, (pat >> (15 - (i & 15))) & 1);
        if (dx > dy) { x0 += sx; err -= dy; if (err < 0) { err += dx; y0 += sy; } }
        else         { y0 += sy; err -= dx; if (err < 0) { err += dy; x0 += sx; } }
    }
}

/* The rectangle x0..x1, y0..y1 (inclusive), solid pattern. Nothing when x1 < x0 or y1 < y0. */
void gfx_RectFill(APTR rp, s16 x0, s16 y0, s16 x1, s16 y1)
{
    if (x1 < x0 || y1 < y0) return;
    Pen p;
    if (!pen_setup(rp, &p)) return;
    for (s32 y = y0; y <= y1; y++)
        for (s32 x = x0; x <= x1; x++) pen_pixel(&p, x, y, true);
}

/* n characters at the current position (y = baseline), 8 pixels each; cp_x advances by 8 * n. */
void gfx_Text(APTR rp, const char *s, u16 n)
{
    s32 x = (s16)rd16(rp + RP_cp_x), y = (s16)rd16(rp + RP_cp_y) - FONT8_BASELINE;
    Pen p;
    bool ok = pen_setup(rp, &p);
    for (u16 i = 0; i < n; i++, x += FONT8_W) {
        if (!ok) continue;
        for (int r = 0; r < FONT8_H; r++) {
            u8 bits = font8_row((u8)s[i], r);
            for (int c = 0; c < FONT8_W; c++) pen_pixel(&p, x + c, y + r, (bits << c) & 0x80);
        }
    }
    wr16(rp + RP_cp_x, (u16)x);
}

u16 gfx_TextLength(APTR rp, const char *s, u16 n)
{
    (void)rp; (void)s;
    return (u16)(n * FONT8_W);
}

/* Scrolls the rectangle x0..x1, y0..y1 by (-dx, -dy): each pixel takes the one dx right and dy below it (dy > 0
 * moves the contents up), and what comes from outside the rectangle is BgPen. Only the planes in RP_Mask. */
void gfx_ScrollRaster(APTR rp, s16 dx, s16 dy, s16 x0, s16 y0, s16 x1, s16 y1)
{
    Pen p;
    if (!pen_setup(rp, &p)) return;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 >= (s32)p.width) x1 = (s16)(p.width - 1);
    if (y1 >= (s32)p.rows) y1 = (s16)(p.rows - 1);
    if (x1 < x0 || y1 < y0) return;
    s32 w = x1 - x0 + 1, h = y1 - y0 + 1;
    static u8 buf[640 * 512];                           /* one plane of the rectangle, a byte per pixel */
    if (w * h > (s32)sizeof buf) return;
    for (u8 i = 0; i < p.depth; i++) {
        if (!(p.mask & (1u << i))) continue;
        bool bgbit = (p.bg >> i) & 1;
        for (s32 y = 0; y < h; y++)
            for (s32 x = 0; x < w; x++) {
                s32 sx = x + dx, sy = y + dy;
                buf[y * w + x] = (sx >= 0 && sx < w && sy >= 0 && sy < h) ? pen_get(&p, i, x0 + sx, y0 + sy) : bgbit;
            }
        for (s32 y = 0; y < h; y++)
            for (s32 x = 0; x < w; x++) {
                APTR a = p.planes[i] + (u32)(y0 + y) * p.bpr + (u32)((x0 + x) >> 3);
                u8 m = (u8)(0x80 >> ((x0 + x) & 7));
                wr8(a, buf[y * w + x] ? rd8(a) | m : rd8(a) & (u8)~m);
            }
    }
}

/* ---------------------------------------------------------------- fonts */

static APTR the_font;                                    /* the shared "ROM" font */

APTR gfx_OpenFont(APTR textattr)
{
    (void)textattr;                                      /* the game asks for topaz 8 only */
    if (!the_font) {
        the_font = exec_AllocMem(TF_SIZE, MEMF_PUBLIC | MEMF_CLEAR);
        wr16(the_font + TF_YSize, FONT8_H);
        wr8(the_font + TF_Flags, FPF_ROMFONT | FPF_DESIGNED);
        wr16(the_font + TF_XSize, FONT8_W);
        wr16(the_font + TF_Baseline, FONT8_BASELINE);
        wr16(the_font + TF_BoldSmear, 1);
        wr8(the_font + TF_LoChar, 0x20);
        wr8(the_font + TF_HiChar, 0xFF);
    }
    wr16(the_font + TF_Accessors, (u16)(rd16(the_font + TF_Accessors) + 1));
    return the_font;
}

void gfx_CloseFont(APTR font)
{
    if (font && font == the_font && rd16(font + TF_Accessors)) wr16(font + TF_Accessors, (u16)(rd16(font + TF_Accessors) - 1));
}

void gfx_SetFont(APTR rp, APTR font)
{
    if (!font) return;
    wr32(rp + RP_Font, font);
    wr16(rp + RP_TxHeight, rd16(font + TF_YSize));
    wr16(rp + RP_TxWidth, rd16(font + TF_XSize));
    wr16(rp + RP_TxBaseline, rd16(font + TF_Baseline));
    wr16(rp + RP_TxSpacing, 0);
}
