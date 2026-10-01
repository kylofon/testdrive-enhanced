#pragma once
/* Shapes, the blitter and text — port of td 0x10B22-0x11552 (blitter routines, masks, arena), 0x14984,
 * 0x15472 (find_shape), 0x13250-0x134F2 (text) (port/amiga/spec/platform_video.md §2.3, §2.4, §4.6, §4.7;
 * shapes and archives: FORMATS.md, Amiga section).
 *
 * The blitter routines program an emulated blitter (A/B/C/D channels, shifts, first/last word masks, modulos,
 * minterm) that runs synchronously on the BLTSIZE write, so the routines port one to one and blit_wait returns
 * at once. Shapes, archives and planes are in amem[]. */
#include "../amem.h"

/* ---- blitter */
void blit_begin(void);                                          /* 0x10D14 */
void blit_wait(void);                                           /* 0x10E78 */
void blit_set_dest(APTR planes5);                               /* 0x10E26: address of 5 plane pointers */
void blit_shape_word(APTR s, APTR mask, s16 x, s16 y);          /* 0x10E3A: x & ~15; mask 0 = copy */
void blit_shape(APTR s, APTR mask, s16 x, s16 y);               /* 0x10E58: pixel-precise */
void blit_shape_xor(APTR s, s16 x, s16 y);                      /* 0x111E6 */
void blit_shape_xor_word(APTR s, s16 x, s16 y);                 /* 0x11432 */
void blit_clear_rect(APTR plane, s16 x, s16 y, s16 wbytes, s16 h);   /* 0x10D72 */
void blit_fill_rect(APTR plane, s16 x, s16 y, s16 wbytes, s16 h);    /* 0x10DBE */
void set_clip_full(void);                                       /* 0x10FA4 */
void set_clip(s16 top, s16 bottom, s16 left, s16 right);        /* 0x10FBC */
void gfx_OwnBlitter(void);                                      /* graphics glue: nothing to do in the port */
void gfx_DisownBlitter(void);

/* ---- shapes */
APTR arena_new(s32 size);                                       /* 0x10BA8 */
APTR arena_alloc(APTR arena, s32 n);                            /* 0x10BDC */
s32  mask_size(APTR s);                                         /* 0x10C3E */
APTR make_mask(APTR arena, APTR s);                             /* 0x10C5A */
APTR find_shape(APTR archive, u32 name4);                       /* 0x15472: 0 when missing */
void draw_shape_on_view(APTR s, APTR view);                     /* 0x14984 */

/* ---- text (RastPort D:2876, topaz 8) */
void text_init(void);                                           /* 0x13250 */
void text_shutdown(void);                                       /* 0x132E6 */
void text_print(const char *s);                                 /* 0x13306 */
void text_move(s16 x, s16 y);                                   /* 0x13330: y = baseline */
void text_set_pen(s16 c);                                       /* 0x13354 */
void draw_box_outline(s16 x0, s16 y0, s16 x1, s16 y1);          /* 0x1336E */
void erase_rect(s16 x0, s16 y0, s16 x1, s16 y1);                /* 0x1343A */
void text_cursor(void);                                         /* 0x134F2 */

/* ---- graphics.library RastPort drawing (arast.c) */
void gfx_SetAPen(APTR rp, u8 pen);
void gfx_SetBPen(APTR rp, u8 pen);
void gfx_SetDrMd(APTR rp, u8 mode);                             /* JAM1 0, JAM2 1, COMPLEMENT 2, INVERSVID 4 */
void gfx_Move(APTR rp, s16 x, s16 y);
void gfx_Draw(APTR rp, s16 x, s16 y);
void gfx_RectFill(APTR rp, s16 x0, s16 y0, s16 x1, s16 y1);
void gfx_Text(APTR rp, const char *s, u16 n);                   /* the port's topaz-8-style font */
u16  gfx_TextLength(APTR rp, const char *s, u16 n);
void gfx_ScrollRaster(APTR rp, s16 dx, s16 dy, s16 x0, s16 y0, s16 x1, s16 y1);
/* OpenFont always gives the port's topaz-8-style font (a shared TextFont in amem[]: YSize 8, XSize 8, Baseline 6);
 * SetFont also sets the RastPort's TxHeight/TxWidth/TxBaseline, as graphics.library does. */
APTR gfx_OpenFont(APTR textattr);
void gfx_CloseFont(APTR font);
void gfx_SetFont(APTR rp, APTR font);
