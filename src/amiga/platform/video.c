/* Display and pictures — port of td 0x115BC, 0x11756, 0x1C62E, 0x1063A-0x10AA2, 0x15344, 0x1574E-0x1587C
 * (port/amiga/spec/platform_video.md §4.2-§4.5, title_select.md show_view). */
#include "platform.h"

#include "../agfx.h"
#include "../ahost.h"
#include "../asymbols.h"

#define PLANE_BYTES 0x1F40u                             /* 320 x 200 / 8 */

void display_init(void)
{
    gfx_WaitTOF();
    gfx_WaitTOF();
    SETD32(D_saved_actiview, gfx_ActiView());
    APTR viewA = DADDR(D_g_viewA), viewB = DADDR(D_g_viewB);
    APTR vpA = DADDR(D_g_vpA), vpB = DADDR(D_g_vpB);
    APTR bmA = DADDR(D_g_bitmapA), bmB = DADDR(D_g_bitmapB);
    APTR riA = DADDR(D_rasinfoA), riB = DADDR(D_rasinfoB);
    gfx_InitView(viewA);
    gfx_InitView(viewB);
    gfx_InitVPort(vpA);
    gfx_InitVPort(vpB);
    wr32(viewA + VIEW_ViewPort, vpA);
    wr32(viewB + VIEW_ViewPort, vpB);
    gfx_InitBitMap(bmA, 5, 0x140, 0xC8);
    gfx_InitBitMap(bmB, 5, 0x140, 0xC8);
    wr32(riA + RI_BitMap, bmA); wr16(riA + RI_RxOffset, 0); wr16(riA + RI_RyOffset, 0); wr32(riA + RI_Next, 0);
    wr32(riB + RI_BitMap, bmB); wr16(riB + RI_RxOffset, 0); wr16(riB + RI_RyOffset, 0); wr32(riB + RI_Next, 0);
    wr16(vpA + VP_DWidth, 0x140); wr16(vpA + VP_DHeight, 0xC8); wr32(vpA + VP_RasInfo, riA); wr16(vpA + VP_Modes, 0);
    wr16(vpB + VP_DWidth, 0x140); wr16(vpB + VP_DHeight, 0xC8); wr32(vpB + VP_RasInfo, riB); wr16(vpB + VP_Modes, 0);
    gfx_InitRastPort(DADDR(D_g_rpA));
    wr32(DADDR(D_g_rpA) + RP_BitMap, bmA);
    gfx_InitRastPort(DADDR(D_g_rpB));
    wr32(DADDR(D_g_rpB) + RP_BitMap, bmB);
    SETD32(D_colormapA, gfx_GetColorMap(0x20));
    SETD32(D_colormapB, gfx_GetColorMap(0x20));
    wr32(vpA + VP_ColorMap, D32(D_colormapA));
    wr32(vpB + VP_ColorMap, D32(D_colormapB));
    for (int i = 0; i < 5; i++) {
        wr32(bmA + BM_Planes + 4u * i, alloc_chip(PLANE_BYTES));
        wr32(bmB + BM_Planes + 4u * i, alloc_chip(PLANE_BYTES));
    }
    gfx_MakeVPort(viewA, vpA);
    gfx_MakeVPort(viewB, vpB);
    gfx_MrgCop(viewA);
    gfx_MrgCop(viewB);
    /* no LoadView here: the first show_view() puts a View on screen */
}

void display_shutdown(void)
{
    APTR bmA = DADDR(D_g_bitmapA), bmB = DADDR(D_g_bitmapB);
    show_view(D32(D_saved_actiview));
    for (int i = 0; i < 5; i++) {
        free_mem(rd32(bmA + BM_Planes + 4u * i));
        free_mem(rd32(bmB + BM_Planes + 4u * i));
    }
    gfx_FreeColorMap(D32(D_colormapA));
    gfx_FreeColorMap(D32(D_colormapB));
    gfx_FreeVPortCopLists(DADDR(D_g_vpA));
    gfx_FreeVPortCopLists(DADDR(D_g_vpB));
}

void show_view(APTR v)
{
    gfx_LoadView(v);
    SETD32(D_g_frontView, v);
    SETD32(D_g_backView, DADDR(D_g_viewB));
    SETD32(D_g_frontRastPort, DADDR(D_g_rpA));
    SETD32(D_g_backRastPort, DADDR(D_g_rpB));
    if (v == DADDR(D_g_viewB)) {                        /* any other View (the startup one too) makes B the back */
        SETD32(D_g_backView, DADDR(D_g_viewA));
        SETD32(D_g_frontRastPort, DADDR(D_g_rpB));
        SETD32(D_g_backRastPort, DADDR(D_g_rpA));
    }
}

/* ---------------------------------------------------------------- ILBM */

#define ID(a, b, c, d) ((u32)(a) << 24 | (u32)(b) << 16 | (u32)(c) << 8 | (u32)(d))

void iff_next_chunk(APTR base, s32 *off)          /* 0x10692 */
{
    *off = (s32)((rd32(base + (u32)*off + 4) + (u32)*off + 9) & ~1u);
}

void cmap_to_rgb4(APTR ch, u16 *pal)              /* 0x107B4 */
{
    for (int k = 0; k < 0x20; k += 4)
        for (int j = 0; j < 3; j++) pal[k + j] = D16(D_cmap_default + 2 * j);   /* 02F2 0F57 022F */
    s16 n = (s16)((s32)rd32(ch + 4) / 3);
    if (n > 0x20) n = 0x20;
    APTR p = ch + 8;
    for (s16 i = 0; i < n; i++, p += 3)
        pal[i] = (u16)(((rd8(p) << 4) & 0xF00) | ((rd8(p + 2) >> 4) & 0x0F) | (rd8(p + 1) & 0xF0));
}

static void cmp2_to_rgb4(APTR ch)                        /* 0x1086E: the custom second-palette chunk */
{
    s16 n = (s16)((s32)rd32(ch + 4) / 3);                /* the 4 header bytes are counted too */
    if (n > 0x20) n = 0x20;
    SETD16(D_cmp2_line, rd16(ch + 8));
    APTR p = ch + 0xC;
    for (s16 i = 0; i < n; i++, p += 3)
        SETD16(D_cmp2_palette + 2 * i, ((rd8(p) << 4) & 0xF00) | ((rd8(p + 2) >> 4) & 0x0F) | (rd8(p + 1) & 0xF0));
}

void unpack_byterun1_row(APTR *src, APTR *dst, s16 n)   /* 0x15344 */
{
    s16 done = 0;
    while (done < n) {
        s16 c = (s8)rd8((*src)++);
        if (c >= 0) {
            for (s16 k = 0; k <= c; k++) { done++; wr8((*dst)++, rd8((*src)++)); }
        } else if (c != -0x80) {
            u8 b = rd8((*src)++);
            for (s16 k = 0; k < 1 - c; k++) { done++; wr8((*dst)++, b); }
        }
    }
}

static void ilbm_decode_body(APTR bmhd, const u16 *pal, APTR body, APTR view)   /* 0x10906 */
{
    APTR pl[8] = { 0 };
    APTR bm = rd32(rd32(rd32(view + VIEW_ViewPort) + VP_RasInfo) + RI_BitMap);
    u8 nplanes = rd8(bmhd + 8);
    for (u16 i = 0; i < nplanes && i < 8; i++) pl[i] = rd32(bm + BM_Planes + 4u * i);
    u16 rowbytes = (u16)((rd16(bmhd) + 7) >> 3);
    for (u16 r = 0; r < rd16(bmhd + 2); r++)
        for (u16 i = 0; i < nplanes && i < 8; i++) unpack_byterun1_row(&body, &pl[i], (s16)rowbytes);
    gfx_WaitTOF();
    APTR tmp = exec_AllocMem(0x40, MEMF_PUBLIC);         /* LoadRGB4 reads the colours from 68000 memory */
    for (int i = 0; i < 32; i++) wr16(tmp + 2u * i, pal[i]);
    gfx_LoadRGB4(rd32(view + VIEW_ViewPort), tmp, 0x20);
    exec_FreeMem(tmp, 0x40);
}

static void ilbm_parse(APTR form, APTR view)             /* 0x106BE */
{
    APTR bmhd = 0, body = 0;
    u16 pal[32] = { 0 };                                /* the original's is uninitialised without a CMAP */
    s32 off = 0xC, end = (s32)rd32(form + 4) + 8;
    while (off < end) {
        APTR ch = form + (u32)off;
        switch (rd32(ch)) {
        case ID('C', 'M', 'A', 'P'): cmap_to_rgb4(ch, pal); break;
        case ID('C', 'M', 'P', '2'): cmp2_to_rgb4(ch); break;
        case ID('B', 'M', 'H', 'D'): bmhd = ch + 8; break;
        case ID('B', 'O', 'D', 'Y'): body = ch + 8; break;
        default: break;
        }
        iff_next_chunk(form, &off);
        if (off >= 0x9C40 || off <= 0) break;            /* 40000-byte safety limit */
    }
    if (bmhd && body) ilbm_decode_body(bmhd, pal, body, view);
}

void ilbm_to_view(APTR buf, APTR view)
{
    view_clear(view);
    if (rd32(buf) == ID('F', 'O', 'R', 'M') && rd32(buf + 8) == ID('I', 'L', 'B', 'M')) ilbm_parse(buf, view);
}

/* ---------------------------------------------------------------- clear, copy */

void view_clear(APTR view)
{
    APTR zero = exec_AllocMem(0x40, MEMF_PUBLIC | MEMF_CLEAR);
    for (APTR vp = rd32(view + VIEW_ViewPort); vp; vp = rd32(vp + VP_Next)) {
        APTR bm = rd32(rd32(vp + VP_RasInfo) + RI_BitMap);
        u16 n = (u16)(rd16(bm + BM_Rows) * rd16(bm + BM_BytesPerRow));
        gfx_LoadRGB4(vp, zero, (u16)(1u << rd8(bm + BM_Depth)));
        for (u16 i = 0; i < rd8(bm + BM_Depth); i++) gfx_BltClear(rd32(bm + BM_Planes + 4u * i), n, 1);
    }
    exec_FreeMem(zero, 0x40);
}

void view_copy_palette(APTR src, APTR dst)
{
    APTR s = rd32(src + VIEW_ViewPort), d = rd32(dst + VIEW_ViewPort);
    for (;;) {
        APTR cm = rd32(s + VP_ColorMap);
        gfx_LoadRGB4(d, rd32(cm + CM_ColorTable), rd16(cm + CM_Count));
        if (!rd32(s + VP_Next) || !rd32(d + VP_Next)) break;
        s = rd32(s + VP_Next);
        d = rd32(d + VP_Next);
    }
}

void view_copy_bitmaps(APTR src, APTR dst)
{
    APTR s = rd32(src + VIEW_ViewPort), d = rd32(dst + VIEW_ViewPort);
    for (;;) {
        APTR a = rd32(rd32(s + VP_RasInfo) + RI_BitMap), b = rd32(rd32(d + VP_RasInfo) + RI_BitMap);
        gfx_BltBitMap(a, 0, 0, b, 0, 0, (s16)(rd16(a + BM_BytesPerRow) * 8), (s16)rd16(a + BM_Rows), 0xCC, 0xFF);
        if (!rd32(s + VP_Next) || !rd32(d + VP_Next)) break;
        s = rd32(s + VP_Next);
        d = rd32(d + VP_Next);
    }
}

void view_copy(APTR src, APTR dst)
{
    view_copy_palette(src, dst);
    view_copy_bitmaps(src, dst);
}

/* ---------------------------------------------------------------- dissolve */

static const u32 dissolve_bits[8] = { 0x01010101, 0x08080808, 0x40404040, 0x02020202,
                                      0x10101010, 0x80808080, 0x04040404, 0x20202020 };   /* 0x10A82 */

/* The original runs at CPU speed with no frame sync, so the picture changes while it is shown. The port paces
 * each pass by the 68000 estimate of platform_video §6 (about 1.8 s for a reveal, 0.9 s to black, 8 passes),
 * pumping the host between slices of rows so the dissolve is seen. */
#define DISSOLVE_SLICES 8

static void dissolve_pass(s16 k, u16 depth, const APTR *s, const APTR *d, bool clear, u64 pass_ns)   /* 0x10AA2 */
{
    u64 start = ahost_time_ns();
    u16 off = 0;
    for (s16 r = 0; r < 0xC8; r++) {
        u32 m = dissolve_bits[(r + k) & 7];
        for (s16 l = 0; l < 10; l++)
            for (u16 p = 0; p < depth; p++) {
                APTR dp = d[p] + off + 4u * (u16)l;
                u32 v = rd32(dp) & ~m;
                if (!clear) v |= rd32(s[p] + off + 4u * (u16)l) & m;
                wr32(dp, v);
            }
        off = (u16)((off + 0x118) % 0x1F40);               /* next row = (7 x r) mod 200 */
        if ((r + 1) % (0xC8 / DISSOLVE_SLICES) == 0)
            ahost_wait_until_ns(start + pass_ns * (u64)((r + 1) / (0xC8 / DISSOLVE_SLICES)) / DISSOLVE_SLICES);
    }
}

void view_dissolve(APTR src, APTR dst)
{
    if (!dst) return;
    APTR db = rd32(rd32(rd32(dst + VIEW_ViewPort) + VP_RasInfo) + RI_BitMap);
    u16 depth = rd8(db + BM_Depth);
    APTR d[8] = { 0 }, s[8] = { 0 };
    APTR sb = src ? rd32(rd32(rd32(src + VIEW_ViewPort) + VP_RasInfo) + RI_BitMap) : 0;
    for (u16 i = 0; i < depth && i < 8; i++) {
        d[i] = rd32(db + BM_Planes + 4u * i);
        if (src) s[i] = rd32(sb + BM_Planes + 4u * i);
    }
    u64 pass_ns = src ? 225000000u : 112500000u;
    for (s16 k = 7; k >= 0; k--) dissolve_pass(k, depth, s, d, src == 0, pass_ns);
}
