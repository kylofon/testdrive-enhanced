/* graphics.library and the display hardware: see agfx.h. */
#include "agfx.h"

#include <string.h>

#include "ahost.h"
#include "platform/blit.h"

#define VP_HIDE 0x2000

/* ---------------------------------------------------------------- structure init */

void gfx_InitView(APTR v)
{
    amem_set(v, 0, VIEW_SIZE);
}

void gfx_InitVPort(APTR vp)
{
    amem_set(vp, 0, VP_SIZE);
}

void gfx_InitBitMap(APTR bm, u8 depth, u16 width, u16 height)
{
    wr16(bm + BM_BytesPerRow, (u16)(((width + 15) >> 4) << 1));
    wr16(bm + BM_Rows, height);
    wr8(bm + BM_Flags, 0);
    wr8(bm + BM_Depth, depth);
    wr16(bm + 6, 0);
}

void gfx_InitRastPort(APTR rp)
{
    amem_set(rp, 0, RP_SIZE);
    wr8(rp + RP_Mask, 0xFF);
    wr8(rp + RP_FgPen, 0xFF);
    wr8(rp + RP_AOlPen, 0xFF);
    wr8(rp + RP_DrawMode, 1);                          /* JAM2 */
    wr16(rp + RP_LinePtrn, 0xFFFF);
    gfx_SetFont(rp, gfx_OpenFont(0));                  /* the default font (topaz 8) */
}

/* graphics.library 1.x default colour table (the Workbench 1.x colours, then greys). */
static const u16 default_colors[32] = {
    0x000, 0xF00, 0x0F0, 0xFF0, 0x00F, 0xF0F, 0x0FF, 0xFFF, 0x620, 0xE50, 0x9F1, 0xEB0, 0x55F, 0x92F, 0x0F8, 0xCCC,
    0x000, 0xD22, 0x000, 0xFCA, 0x444, 0x555, 0x666, 0x777, 0x888, 0x999, 0xAAA, 0xBBB, 0xCCC, 0xDDD, 0xEEE, 0xFFF,
};

APTR gfx_GetColorMap(u16 entries)
{
    APTR cm = exec_AllocMem(CM_SIZE, MEMF_PUBLIC | MEMF_CLEAR);
    APTR table = exec_AllocMem((u32)entries * 2, MEMF_PUBLIC | MEMF_CLEAR);
    if (!cm || !table) ahost_fatal("Out of memory for a colour map.");
    wr16(cm + CM_Count, entries);
    wr32(cm + CM_ColorTable, table);
    for (u16 i = 0; i < entries; i++) wr16(table + 2u * i, default_colors[i & 31]);
    return cm;
}

void gfx_FreeColorMap(APTR cm)
{
    if (!cm) return;
    exec_FreeMem(rd32(cm + CM_ColorTable), (u32)rd16(cm + CM_Count) * 2);
    exec_FreeMem(cm, CM_SIZE);
}

void gfx_LoadRGB4(APTR vp, APTR colors, u16 count)
{
    APTR cm = rd32(vp + VP_ColorMap);
    if (!cm) return;
    u16 n = rd16(cm + CM_Count);
    APTR table = rd32(cm + CM_ColorTable);
    for (u16 i = 0; i < count && i < n; i++) wr16(table + 2u * i, rd16(colors + 2u * i) & 0x0FFF);
}

void gfx_SetRGB4(APTR vp, u16 n, u8 r, u8 g, u8 b)
{
    APTR cm = rd32(vp + VP_ColorMap);
    if (!cm || n >= rd16(cm + CM_Count)) return;
    wr16(rd32(cm + CM_ColorTable) + 2u * n, (u16)((r & 15) << 8 | (g & 15) << 4 | (b & 15)));
}

u16 gfx_GetRGB4(APTR cm, u16 n)
{
    if (!cm || n >= rd16(cm + CM_Count)) return 0xFFFF;
    return rd16(rd32(cm + CM_ColorTable) + 2u * n);
}

/* ---------------------------------------------------------------- user copper lists */

/* Port-side buffer behind UCopList.FirstCopList: u16 count, u16 capacity, then 6-byte instructions
 * (u16 kind: 0 wait / 1 move, u16 a: line or register, u16 b: horizontal position or value). */
#define UCB_HEAD  4
#define UCB_INS   6
#define COP_WAIT  0
#define COP_MOVE  1

static APTR ucl_buffer(APTR ucl, u16 need)
{
    APTR buf = rd32(ucl + UCL_FirstCopList);
    u16 cap = buf ? rd16(buf + 2) : 0;
    if (need <= cap) return buf;
    u16 ncap = cap ? (u16)(cap * 2) : 64;
    while (ncap < need) ncap = (u16)(ncap * 2);
    APTR nbuf = exec_AllocMem(UCB_HEAD + (u32)ncap * UCB_INS, MEMF_PUBLIC | MEMF_CLEAR);
    if (!nbuf) ahost_fatal("Out of memory for a copper list.");
    if (buf) {
        for (u32 i = 0; i < UCB_HEAD + (u32)cap * UCB_INS; i++) wr8(nbuf + i, rd8(buf + i));
        exec_FreeMem(buf, UCB_HEAD + (u32)cap * UCB_INS);
    }
    wr16(nbuf + 2, ncap);
    wr32(ucl + UCL_FirstCopList, nbuf);
    wr32(ucl + UCL_CopList, nbuf);
    return nbuf;
}

static void ucl_put(APTR ucl, u16 kind, u16 a, u16 b)
{
    APTR buf = rd32(ucl + UCL_FirstCopList);
    u16 count = buf ? rd16(buf) : 0;
    buf = ucl_buffer(ucl, (u16)(count + 1));
    APTR ins = buf + UCB_HEAD + (u32)count * UCB_INS;
    wr16(ins, kind);
    wr16(ins + 2, a);
    wr16(ins + 4, b);
}

void gfx_CWait(APTR ucl, s16 v, s16 h) { ucl_put(ucl, COP_WAIT, (u16)v, (u16)h); }
void gfx_CMove(APTR ucl, u32 reg, u16 value) { ucl_put(ucl, COP_MOVE, (u16)(reg & 0x1FE), value); }

void gfx_CBump(APTR ucl)
{
    APTR buf = rd32(ucl + UCL_FirstCopList);
    if (buf) wr16(buf, (u16)(rd16(buf) + 1));
}

void gfx_FreeVPortCopLists(APTR vp)
{
    /* graphics.library frees the ViewPort's copper lists and its whole UCopList chain. */
    for (APTR ucl = rd32(vp + VP_UCopIns); ucl;) {
        APTR next = rd32(ucl + UCL_Next);
        APTR buf = rd32(ucl + UCL_FirstCopList);
        if (buf) exec_FreeMem(buf, UCB_HEAD + (u32)rd16(buf + 2) * UCB_INS);
        exec_FreeMem(ucl, UCL_SIZE);
        ucl = next;
    }
    wr32(vp + VP_UCopIns, 0);
}

void gfx_FreeCprList(APTR cprlist) { (void)cprlist; }

/* ---------------------------------------------------------------- display lists */

#define MAX_VPS 4
#define MAX_COP 128
#define MAX_SNAPS 4

typedef struct { u8 kind; s16 a; u16 b; } CopIns;

typedef struct {
    s16 dx, dy, rx, ry;
    u16 dw, dh, modes, bpr, rows;
    u8 depth;
    APTR cm;
    APTR planes[8];
    int ncop;
    CopIns cop[MAX_COP];
} VpSnap;

typedef struct { APTR view; int nvp; VpSnap vp[MAX_VPS]; } ViewSnap;

static ViewSnap snaps[MAX_SNAPS];
static APTR loaded_view, shown_view;

static ViewSnap *snap_of(APTR view, bool create)
{
    for (int i = 0; i < MAX_SNAPS; i++) if (snaps[i].view == view) return &snaps[i];
    if (!create) return NULL;
    for (int i = 0; i < MAX_SNAPS; i++)
        if (!snaps[i].view) { snaps[i].view = view; return &snaps[i]; }
    snaps[0].view = view;                              /* more than 4 Views: reuse (the game has 2) */
    return &snaps[0];
}

void gfx_MakeVPort(APTR view, APTR vp) { (void)view; (void)vp; }

void gfx_MrgCop(APTR view)
{
    ViewSnap *s = snap_of(view, true);
    s->nvp = 0;
    for (APTR vp = rd32(view + VIEW_ViewPort); vp && s->nvp < MAX_VPS; vp = rd32(vp + VP_Next)) {
        u16 modes = rd16(vp + VP_Modes);
        APTR ri = rd32(vp + VP_RasInfo);
        APTR bm = ri ? rd32(ri + RI_BitMap) : 0;
        if ((modes & VP_HIDE) || !bm) continue;
        VpSnap *p = &s->vp[s->nvp++];
        p->dx = (s16)rd16(vp + VP_DxOffset);
        p->dy = (s16)rd16(vp + VP_DyOffset);
        p->dw = rd16(vp + VP_DWidth);
        p->dh = rd16(vp + VP_DHeight);
        p->modes = modes;
        p->rx = (s16)rd16(ri + RI_RxOffset);
        p->ry = (s16)rd16(ri + RI_RyOffset);
        p->bpr = rd16(bm + BM_BytesPerRow);
        p->rows = rd16(bm + BM_Rows);
        p->depth = rd8(bm + BM_Depth) > 6 ? 6 : rd8(bm + BM_Depth);
        for (int i = 0; i < 8; i++) p->planes[i] = i < p->depth ? rd32(bm + BM_Planes + 4u * i) : 0;
        p->cm = rd32(vp + VP_ColorMap);
        p->ncop = 0;
        for (APTR ucl = rd32(vp + VP_UCopIns); ucl; ucl = rd32(ucl + UCL_Next)) {
            APTR buf = rd32(ucl + UCL_FirstCopList);
            u16 n = buf ? rd16(buf) : 0;
            for (u16 i = 0; i < n && p->ncop < MAX_COP; i++) {
                APTR ins = buf + UCB_HEAD + (u32)i * UCB_INS;
                p->cop[p->ncop++] = (CopIns){ (u8)rd16(ins), (s16)rd16(ins + 2), rd16(ins + 4) };
            }
        }
    }
}

void gfx_LoadView(APTR view)
{
    if (view && !snap_of(view, false)) gfx_MrgCop(view);
    loaded_view = view;
}

APTR gfx_ActiView(void) { return loaded_view; }

void gfx_WaitTOF(void) { ahost_wait_tof(); }

/* The copper reloads at the vertical blank: a LoadView shows from the next frame. */
static void display_vbl(void) { shown_view = loaded_view; }

/* ---------------------------------------------------------------- hardware sprites */

/* Sprite DMA fetches each sprite's data from SPRxPT (loaded by the user copper list at the top of the frame).
 * The data is a chain of *segments*: two control words (SPRxPOS, SPRxCTL) then one DATA and one DATB word per
 * line, lines VSTART..VSTOP-1; a zero control pair ends the chain (drive_scene.md §4.8.1). Position 0x80/0x2C
 * is the top left of the display window. A pair 2n/2n+1 is two 3-colour sprites in COLOR(17+4n)..COLOR(19+4n),
 * the even one in front; when the odd one's ATT bit is set the pair is one 15-colour sprite in COLOR17..31
 * (bits: even DATA, even DATB, odd DATA, odd DATB). Sprites are in front of the playfield. */
#define SPR_HSTART0  0x80
#define SPR_VSTART0  0x2C
#define MAX_SPR_SEG  8

typedef struct { s16 y0, y1, x; u8 att; APTR data; } SprSeg;

static SprSeg spr_seg[8][MAX_SPR_SEG];
static int spr_nseg[8];
static bool sprite_dma;

void gfx_dmacon(u16 value)
{
    if (value & 0x0020) sprite_dma = (value & 0x8000) != 0;
}

/* Reads the SPRxPT longs out of the View's copper instructions and follows each sprite's chain. */
static bool sprites_decode(const ViewSnap *s)
{
    for (int n = 0; n < 8; n++) spr_nseg[n] = 0;
    if (!sprite_dma || !s) return false;
    APTR ptr[8] = { 0 };
    bool any = false;
    for (int i = 0; i < s->nvp; i++)
        for (int k = 0; k < s->vp[i].ncop; k++) {
            const CopIns *c = &s->vp[i].cop[k];
            if (c->kind != COP_MOVE) continue;
            u16 reg = (u16)c->a;
            if (reg < CUSTOM_SPR0PT || reg >= CUSTOM_SPR0PT + 32) continue;
            int n = (reg - CUSTOM_SPR0PT) >> 2;
            if (reg & 2) ptr[n] = (ptr[n] & 0xFFFF0000u) | c->b;
            else         ptr[n] = (ptr[n] & 0x0000FFFFu) | ((u32)c->b << 16);
            any = true;
        }
    if (!any) return false;
    bool some = false;
    for (int n = 0; n < 8; n++) {
        APTR p = ptr[n] & ~1u;
        while (p >= 4 && p + 4 <= AMEM_SIZE && spr_nseg[n] < MAX_SPR_SEG) {
            u16 pos = rd16(p), ctl = rd16(p + 2);
            if (!pos && !ctl) break;
            s16 v0 = (s16)(((ctl & 4) << 6) | (pos >> 8));
            s16 v1 = (s16)(((ctl & 2) << 7) | (ctl >> 8));
            s16 lines = (s16)(v1 - v0);
            if (lines <= 0 || lines > 0x100 || p + 4 + 4u * (u32)lines > AMEM_SIZE) break;
            SprSeg *g = &spr_seg[n][spr_nseg[n]++];
            g->y0 = (s16)(v0 - SPR_VSTART0);
            g->y1 = (s16)(v1 - SPR_VSTART0);
            g->x = (s16)((((pos & 0xFF) << 1) | (ctl & 1)) - SPR_HSTART0);
            g->att = (u8)((ctl >> 7) & 1);
            g->data = p + 4;
            p += 4 + 4u * (u32)lines;
            some = true;
        }
    }
    return some;
}

static const SprSeg *spr_at(int n, int y)
{
    for (int k = 0; k < spr_nseg[n]; k++)
        if (y >= spr_seg[n][k].y0 && y < spr_seg[n][k].y1) return &spr_seg[n][k];
    return NULL;
}

/* One scanline of the sprite layer, as 320 colour indices (0 = nothing there). */
static void sprites_row(int y, u8 *out)
{
    memset(out, 0, GFX_FRAME_W / 2);
    for (int pair = 3; pair >= 0; pair--) {         /* pair 0 (sprites 0/1) has the highest priority */
        const SprSeg *e = spr_at(2 * pair, y), *o = spr_at(2 * pair + 1, y);
        if (!e && !o) continue;
        u16 ea = 0, eb = 0, oa = 0, ob = 0;
        if (e) { APTR d = e->data + 4u * (u32)(y - e->y0); ea = rd16(d); eb = rd16(d + 2); }
        if (o) { APTR d = o->data + 4u * (u32)(y - o->y0); oa = rd16(d); ob = rd16(d + 2); }
        bool att = o && o->att;
        s16 x0 = e ? e->x : o->x, x1 = (s16)((e ? e->x : o->x) + 16);
        if (e && o) {
            x0 = e->x < o->x ? e->x : o->x;
            x1 = (s16)((e->x > o->x ? e->x : o->x) + 16);
        }
        for (s16 x = x0; x < x1; x++) {
            if (x < 0 || x >= GFX_FRAME_W / 2) continue;
            int ev = 0, ov = 0;
            if (e && x >= e->x && x < e->x + 16) {
                u16 m = (u16)(0x8000 >> (x - e->x));
                ev = ((ea & m) ? 1 : 0) | ((eb & m) ? 2 : 0);
            }
            if (o && x >= o->x && x < o->x + 16) {
                u16 m = (u16)(0x8000 >> (x - o->x));
                ov = ((oa & m) ? 1 : 0) | ((ob & m) ? 2 : 0);
            }
            int col;
            if (att) { int v = ev | (ov << 2); if (!v) continue; col = 16 + v; }
            else if (ev) col = 16 + 4 * pair + ev;
            else if (ov) col = 16 + 4 * pair + ov;
            else continue;
            out[x] = (u8)col;
        }
    }
}

/* ---------------------------------------------------------------- composition */

static u32 rgb4_to_xrgb(u16 c)
{
    u32 r = (c >> 8) & 15, g = (c >> 4) & 15, b = c & 15;
    return (r * 17) << 16 | (g * 17) << 8 | (b * 17);
}

static void load_palette(u16 *pal, APTR cm)
{
    for (int i = 0; i < 32; i++) pal[i] = 0;
    if (!cm) return;
    u16 n = rd16(cm + CM_Count);
    APTR table = rd32(cm + CM_ColorTable);
    for (u16 i = 0; i < n && i < 32; i++) pal[i] = rd16(table + 2u * i) & 0x0FFF;
}

static void compose(u32 *out)
{
    ViewSnap *s = shown_view ? snap_of(shown_view, false) : NULL;
    if (!s || s->nvp == 0) {
        memset(out, 0, GFX_FRAME_W * GFX_FRAME_H * sizeof *out);
        return;
    }
    u16 pal[32];
    load_palette(pal, s->vp[0].cm);
    const VpSnap *cur = NULL;
    int cop = 0;
    bool sprites = sprites_decode(s);
    u8 sprrow[GFX_FRAME_W / 2];
    for (int y = 0; y < GFX_FRAME_H; y++) {
        u32 *row = out + y * GFX_FRAME_W;
        const VpSnap *p = NULL;
        for (int i = 0; i < s->nvp; i++)
            if (y >= s->vp[i].dy && y < s->vp[i].dy + s->vp[i].dh) { p = &s->vp[i]; break; }
        if (p != cur && p) {                           /* each ViewPort starts with its own colours */
            load_palette(pal, p->cm);
            cop = 0;
        }
        cur = p ? p : cur;
        if (p) {
            /* User copper instructions due by this line. A WAIT on horizontal position > 0x40 (inside the
             * display window) changes the colours partway through line v; the port switches them from v + 1
             * (the game's mid-line switches are hidden under shapes, drive_scene §6). */
            while (cop < p->ncop) {
                const CopIns *c = &p->cop[cop];
                if (c->kind == COP_WAIT) {
                    int line = p->dy + c->a + (c->b > 0x40 ? 1 : 0);
                    if (y < line) break;
                } else if (c->a >= CUSTOM_COLOR00 && c->a < CUSTOM_COLOR00 + 64) {
                    pal[(c->a - CUSTOM_COLOR00) >> 1] = c->b & 0x0FFF;
                }
                cop++;
            }
        }
        u32 xpal[32];
        for (int i = 0; i < 32; i++) xpal[i] = rgb4_to_xrgb(pal[i]);
        if (sprites) sprites_row(y, sprrow);            /* in front of the playfield */
        if (!p) {
            for (int x = 0; x < GFX_FRAME_W; x++)
                row[x] = xpal[sprites && sprrow[x >> 1] ? sprrow[x >> 1] & 31 : 0];
            continue;
        }
        int src_row = p->ry + (y - p->dy);
        bool hires = (p->modes & V_HIRES) != 0;
        for (int x = 0; x < GFX_FRAME_W; x++) {
            int vx = (hires ? x : x >> 1) - p->dx;         /* pixel in the ViewPort */
            int col = vx + p->rx;
            u8 index = 0;
            if (vx >= 0 && vx < p->dw && col >= 0 && col < p->bpr * 8 && src_row >= 0 && src_row < p->rows) {
                u32 off = (u32)src_row * p->bpr + (u32)(col >> 3);
                u8 bit = (u8)(0x80 >> (col & 7));
                for (int pl = 0; pl < p->depth; pl++)
                    if (p->planes[pl] && (rd8(p->planes[pl] + off) & bit)) index |= (u8)(1 << pl);
            }
            if (sprites && sprrow[x >> 1]) index = sprrow[x >> 1];
            row[x] = xpal[index & 31];
        }
    }
}

void gfx_display_init(void)
{
    ahost_add_vbl_server(127, display_vbl);
    ahost_set_frame_source(compose, GFX_FRAME_W, GFX_FRAME_H);
}

/* ---------------------------------------------------------------- blitter library calls */

void gfx_BltClear(APTR mem, u32 bytecount, u32 flags)
{
    /* flags bit 1: bytecount is rows << 16 | bytes per row. */
    u32 n = (flags & 2) ? (bytecount >> 16) * (bytecount & 0xFFFF) : bytecount;
    amem_set(mem, 0, n);
}

void gfx_BltBitMap(APTR src, s16 sx, s16 sy, APTR dst, s16 dx, s16 dy, s16 w, s16 h, u8 minterm, u8 mask)
{
    u16 sbpr = rd16(src + BM_BytesPerRow), dbpr = rd16(dst + BM_BytesPerRow);
    u8 depth = rd8(src + BM_Depth) < rd8(dst + BM_Depth) ? rd8(src + BM_Depth) : rd8(dst + BM_Depth);
    for (u8 p = 0; p < depth && p < 8; p++) {
        if (!(mask & (1 << p))) continue;
        APTR sp = rd32(src + BM_Planes + 4u * p), dp = rd32(dst + BM_Planes + 4u * p);
        for (s16 y = 0; y < h; y++) {
            APTR srow = sp + (u32)(sy + y) * sbpr, drow = dp + (u32)(dy + y) * dbpr;
            if (minterm == 0xC0 || minterm == 0xCC) {
                if (!((sx | dx | w) & 7)) {                /* byte-aligned: copy bytes */
                    for (s16 k = 0; k < w / 8; k++) wr8(drow + (u32)(dx / 8 + k), rd8(srow + (u32)(sx / 8 + k)));
                    continue;
                }
            }
            for (s16 x = 0; x < w; x++) {
                int s = (rd8(srow + (u32)((sx + x) >> 3)) >> (7 - ((sx + x) & 7))) & 1;
                APTR d = drow + (u32)((dx + x) >> 3);
                u8 bit = (u8)(0x80 >> ((dx + x) & 7));
                int dv = (rd8(d) & bit) != 0;
                /* minterm bits (B = source, C = destination): bit 7 BC, 6 Bc, 5 bC, 4 bc. */
                int r = (minterm >> (4 + (s ? 2 : 0) + (dv ? 1 : 0))) & 1;
                wr8(d, r ? (u8)(rd8(d) | bit) : (u8)(rd8(d) & ~bit));
            }
        }
    }
}
