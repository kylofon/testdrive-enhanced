/* Text — port of td 0x13250-0x134F2 (port/amiga/spec/platform_video.md §2.4, §4.7): the RastPort D:2876 with
 * topaz 8 (the port's font, arast.c), pens, the box outline, erase and the name-entry cursor. Callers point
 * D:2876->BitMap at the bitmap they draw into. */
#include "blit.h"

#include "platform.h"
#include "../agfx.h"
#include "../asymbols.h"

#define RP()  D32(D_g_tmpRastPort)

#define STR_TOPAZ_FONT 0x132DAu                          /* "topaz.font" in the code hunk */

void text_init(void)                                     /* 0x13250 */
{
    APTR ta = alloc_public(8);
    wr32(ta, STR_TOPAZ_FONT);                            /* ta_Name */
    wr16(ta + 4, 8);                                     /* ta_YSize */
    SETD32(D_g_tmpRastPort, alloc_public(0x64));
    gfx_InitRastPort(RP());
    wr32(RP() + RP_BitMap, DADDR(D_g_bitmapA));
    SETD32(D_text_font, gfx_OpenFont(ta));
    gfx_SetFont(RP(), D32(D_text_font));
    free_mem(ta);
    text_set_pen(1);
    gfx_SetDrMd(RP(), 0);                                /* JAM1 */
}

void text_shutdown(void)                                 /* 0x132E6 */
{
    gfx_CloseFont(D32(D_text_font));
    free_mem(RP());
    SETD32(D_g_tmpRastPort, 0);
}

void text_print(const char *s)                           /* 0x13306 */
{
    size_t n = 0;
    while (s[n]) n++;
    gfx_Text(RP(), s, (u16)n);                           /* strlen as an int, ext.l */
}

void text_move(s16 x, s16 y) { gfx_Move(RP(), x, y); }   /* 0x13330 */
void text_set_pen(s16 c) { gfx_SetAPen(RP(), (u8)c); }   /* 0x13354 */

void draw_box_outline(s16 x0, s16 y0, s16 x1, s16 y1)    /* 0x1336E */
{
    s8 fg = (s8)rd8(RP() + RP_FgPen);
    gfx_SetAPen(RP(), 1);
    gfx_SetDrMd(RP(), 0);
    gfx_Move(RP(), x0, y0);
    gfx_Draw(RP(), x1, y0);
    gfx_Draw(RP(), x1, y1);
    gfx_Draw(RP(), x0, y1);
    gfx_Draw(RP(), x0, y0);
    gfx_SetAPen(RP(), (u8)fg);
}

void erase_rect(s16 x0, s16 y0, s16 x1, s16 y1)          /* 0x1343A */
{
    u8 mode = rd8(RP() + RP_DrawMode), fg = rd8(RP() + RP_FgPen);
    s16 cx = (s16)rd16(RP() + RP_cp_x), cy = (s16)rd16(RP() + RP_cp_y);
    gfx_SetAPen(RP(), 0);
    gfx_SetDrMd(RP(), 0);
    gfx_RectFill(RP(), x0, y0, x1, y1);
    gfx_SetAPen(RP(), fg);
    gfx_SetDrMd(RP(), mode);
    gfx_Move(RP(), cx, cy);
}

/* 0x134F2: a COMPLEMENT block in pen 0x1F from (cx, cy - 8) to (cx + 8, cy + 1); drawn twice it is gone. */
void text_cursor(void)
{
    u8 mode = rd8(RP() + RP_DrawMode), fg = rd8(RP() + RP_FgPen);
    s16 cx = (s16)rd16(RP() + RP_cp_x), cy = (s16)rd16(RP() + RP_cp_y);
    gfx_SetAPen(RP(), 0x1F);
    gfx_SetDrMd(RP(), 2);                                /* COMPLEMENT */
    gfx_RectFill(RP(), cx, (s16)(cy - 8), (s16)(cx + 8), (s16)(cy + 1));
    gfx_SetAPen(RP(), fg);
    gfx_SetDrMd(RP(), mode);
    gfx_Move(RP(), cx, cy);
}
