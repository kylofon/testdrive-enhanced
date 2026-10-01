/* The drive's picture — port of overlay 2's renderer, td 0x1CD72-0x1FEAE, 0x20096-0x20B1C, 0x246FA-0x247B2 and
 * 0x24D68-0x2627A (port/amiga/spec/drive_scene.md, all of §4).
 *
 * The original draws in a second exec task ("Road Drawer", priority -1) that renders as fast as the CPU allows
 * while the main task runs one simulation step per 5 vertical blanks. The port is single-threaded: the drive
 * flow calls road_drawer_step() once per 60 Hz tick (render one frame, then show_view), and the `Forbid`
 * snapshot of the simulation state becomes a plain copy (drive_scene §5, §6).
 *
 * The road is an incremental fixed-point walker (§1.3): 40 rows forward for the main view, 30 backwards for the
 * mirror, each row a trapezoid filled with one-line blits on single planes and clipped to the crest. Road-side
 * objects go into a 199-entry queue near->far and are drawn far->near. The cockpit shapes, the gauges (hardware
 * sprites, or the Corvette's digital cluster) and the copper palette split at line 0x75 sit on top.
 *
 * Everything keeps the original's 16-bit arithmetic and its globals at their D: addresses, so the simulation
 * (game/sim.c) reads what it expects: the row-0 half-widths D:0D94/D:0D96, the car_x snapshot D:289A and the
 * traffic laterals D:0CC2/D:0D28. The three original defects this spec owns (README bugs 2, 5 and 6) are fixed
 * unless g_original_bugs is set. */
#include "scene.h"

#include "sim.h"
#include "../agfx.h"
#include "../ahost.h"
#include "../aport.h"
#include "../asymbols.h"
#include "../platform/audio_drive.h"
#include "../platform/blit.h"
#include "../platform/fixed.h"
#include "../platform/platform.h"

#define ID(a, b, c, d) ((u32)(a) << 24 | (u32)(b) << 16 | (u32)(c) << 8 | (u32)(d))

/* ---------------------------------------------------------------- globals not in asymbols.h
 * The gauge part of the car-record copy (drive_sim's 0x20D68 fills it; drive_scene §3.1 names the fields), the
 * shape handles of §4.11 and the two leftovers of the dead bump-shake detector. */
#define D_car_tach_clamp        0x1926      /* +002 */
#define D_marker_x              0x1930      /* +00C */
#define D_marker_y              0x1932      /* +00E */
#define D_marker_r              0x1934      /* +010 */
#define D_needle_gauges         0x194E      /* +16C: 0 = the Corvette's digital cluster */
#define D_speed_digit_xy        0x1950      /* +16E, ptr */
#define D_speed_bar_dir         0x1954      /* +17A */
#define D_speed_bar_x0          0x1956      /* +17C */
#define D_speed_bar_scale       0x195A      /* +180 */
#define D_speed_digit_prefix    0x195C      /* +182, long */
#define D_tach_digit_xy         0x1960      /* +31C, ptr */
#define D_tach_bar_dir          0x1964      /* +324 */
#define D_tach_bar_x0           0x1966      /* +326 */
#define D_tach_bar_scale        0x196A      /* +32A */
#define D_tach_digit_prefix     0x196C      /* +32C, long */
#define D_pivot_speed_x         0x1970      /* +16E */
#define D_pivot_speed_y         0x1971      /* +16F */
#define D_pivot_tach_x          0x1972      /* +170 */
#define D_pivot_tach_y          0x1973      /* +171 */
#define D_speed_box_x           0x1974      /* +172 */
#define D_speed_box_y           0x1976      /* +174 */
#define D_tach_box_x            0x1978      /* +176 */
#define D_tach_box_y            0x197A      /* +178 */
#define D_speed_tips            0x197C      /* +17A, ptr */
#define D_tach_tips             0x1980      /* +328, ptr */

#define D_bump_state            0x283A
#define D_last_pitch            0x283E

#define D_sign_mask_table       0x1A6E
#define D_hazard_mask_table     0x1A1E
#define D_car_mask_table        0x1AEE
#define D_mirror_car_mask_table 0x1B6E

/* shape handles (drive_scene §4.11); *_M is the generated mask */
#define S_gbox      0x2514
#define S_bug1_M    0x2518
#define S_bug1      0x251C
#define S_bug1b_M   0x2520
#define S_bug1b     0x2524
#define S_bugX_M    0x2528      /* bugA..bugD */
#define S_bugX      0x2538
#define S_clf0_M    0x254C
#define S_clf0      0x2550
#define S_cld_M     0x2554      /* cldA cldB cld0 cld1 */
#define S_cld       0x2564
#define S_hood      0x2574
#define S_hood_M    0x2578
#define S_gnob      0x257C
#define S_lin_M     0x2580      /* linA..linD */
#define S_lin       0x2590
#define S_pst_M     0x25A0      /* pst1..pst4 */
#define S_pst       0x25B0
#define S_mirr      0x25C0
#define S_mirr_M    0x25C4
#define S_pol_M     0x25C8      /* pol0..pol4 rpl0 rpl1 */
#define S_pol       0x25E4
#define S_post      0x2600
#define S_post_M    0x2604
#define S_rad_M     0x2608      /* rad0..rad5 */
#define S_rad       0x2620
#define S_rsg_M     0x2638      /* rsg0 rsg1 */
#define S_rsg       0x2640
#define S_rck_M     0x2648      /* rckA..rckG (rckH has no mask) */
#define S_rck       0x2668
#define S_roof      0x2688
#define S_roof_M    0x268C
#define S_whl       0x2690      /* whl0..whl3 */
#define S_wed_M     0x26A0      /* wed0..wed3 */
#define S_wed       0x26B0
#define S_cpb       0x26C0      /* [0..2] = 0, cpb3, cpb4 */
#define S_clr       0x26D4      /* clr0..clr4 */
#define S_cpl       0x26E8      /* cpl0..cpl3 cpl3 */
#define S_tcm       0x26FE      /* tcm0 tcm1 tcmC tcm2 tcm3 */
#define S_spm       0x2712
#define S_tcm_M     0x272E
#define S_spm_M     0x2742
#define S_tick      0x275E
#define S_tick_M    0x2762
#define S_inst      0x2772
#define S_sped      0x2776
#define S_tach      0x277A
#define S_tach_dgt  0x277E      /* 10 digits */
#define S_speed_dgt 0x27A6
#define S_oil       0x2962
#define S_pot       0x298A
#define S_gra       0x29B2
#define S_cop       0x2DC2      /* mirror police: cop0..cop3 cop3 */
#define S_cop_M     0x2DD6
#define S_cpr       0x2DEA      /* cpr0..cpr4 */
#define S_cpr_M     0x2DFE

/* ---------------------------------------------------------------- small helpers */

static s16 shape_hot_x(APTR s) { return (s16)rd16(s + 4); }
static s16 shape_hot_y(APTR s) { return (s16)rd16(s + 6); }
static s16 shape_x(APTR s)     { return (s16)rd16(s + 8); }
static s16 shape_y(APTR s)     { return (s16)rd16(s + 0xA); }

/* A Shape * table entry (the tables are indexed by the byte offsets the original keeps in registers; a stray
 * index is sign-extended, as `move.l (a0,d0.w)` does). */
static APTR tbl(u16 base, s16 byteoff) { return rd32(DADDR(base) + (u32)(s32)byteoff); }

static APTR view_planes(APTR view)
{
    return rd32(rd32(rd32(view + VIEW_ViewPort) + VP_RasInfo) + RI_BitMap) + BM_Planes;
}

static APTR dest_plane(int p) { return D32(D_blit_dest + 4 * p); }

/* 0x1D306: (s16)d0 * D:0B62, signed, 32-bit. */
static s32 mul_scale(s16 v)
{
    u16 s = D16(D_row_scale);
    if (v >= 0) return (s32)((u32)(u16)v * (u32)s);
    return -(s32)((u32)(u16)(-(u16)v) * (u32)s);
}

/* 0x1D31A: *(s16 *)a clamped to [lo, hi] (the upper bound first, as the original tests it). */
static void clamp_word(APTR a, s16 lo, s16 hi)
{
    if ((s16)rd16(a) > hi) wr16(a, (u16)hi);
    else if (lo > (s16)rd16(a)) wr16(a, (u16)lo);
}

/* divu with the sign put back by negation, rounding toward 0 (fill_road_row 0x201DC). */
static s16 trunc0(s16 diff, s16 div)
{
    if (diff >= 0) return (s16)((u16)((u32)(u16)diff / (u16)div));
    return (s16)(-(s16)((u16)((u32)(u16)(-diff) / (u16)div)));
}

/* 0x10E12 + 0x110C4: a shape at its own position, pixel-exact. */
static void blit_at_own(APTR s, APTR mask) { blit_shape(s, mask, shape_x(s), shape_y(s)); }

/* 0x10E1C + 0x110C4: a shape with its hot spot at (x, y). A null handle reads a zero hot spot, as the original
 * does, and blit_shape then does nothing. */
static void blit_hot(APTR s, APTR mask, s16 x, s16 y)
{
    blit_shape(s, mask, (s16)(x - shape_hot_x(s)), (s16)(y - shape_hot_y(s)));
}

/* ---------------------------------------------------------------- pixels (0x1D27E, 0x1D2C2) */

static bool pixel_ok(s16 x, s16 y)
{
    return !(x < 0 || x >= 0x140 || y < 10 || y >= DS16(D_crest_y));
}

static APTR pixel_addr(s16 x, s16 y, u8 *bit)
{
    *bit = (u8)(1u << (7 - (x & 7)));
    u16 off = (u16)((u16)(x >> 3) + (u16)((u16)y << 3) + (u16)((u16)y << 5));
    return D32(D_blit_dest + (D16(D_pixel_plane_off) & 0x1C)) + off;
}

static void plot_pixel_set(s16 x, s16 y)
{
    if (!pixel_ok(x, y)) return;
    u8 bit;
    APTR a = pixel_addr(x, y, &bit);
    wr8(a, (u8)(rd8(a) | bit));
}

static void plot_pixel_clear(s16 x, s16 y)
{
    if (!pixel_ok(x, y)) return;
    u8 bit;
    APTR a = pixel_addr(x, y, &bit);
    wr8(a, (u8)(rd8(a) & ~bit));
}

/* ================================================================ sprites (§4.8.1) */

#define CUSTOM_SPR0CTL  0x142

static u16 spr_pos_word(s16 h, s16 v0, s16 v1, s16 att)
{
    (void)v1; (void)att;
    return (u16)(((u16)(v0 & 0xFF) << 8) + (u16)(h >> 1));
}

static u16 spr_ctl_word(s16 h, s16 v0, s16 v1, s16 att)
{
    u16 d = (u16)((u16)(v1 & 0xFF) << 8);
    d = (u16)(d + (u16)((s16)(v0 & 0x100) >> 6));
    d = (u16)(d + (u16)((s16)(v1 & 0x100) >> 7));
    d = (u16)(d + (u16)((att ? 1 : 0) << 7));
    return (u16)(d + (u16)(h & 1));
}

static APTR sprite_data(int n) { return D32(D_sprite_data + 4 * n); }

/* 0x1F05A: the POS/CTL pair of one segment (word offset w in sprite n). */
void sprite_set_segment(u16 w, s16 h, s16 v0, s16 v1, s16 att, int n)
{
    s16 shake = DS16(D_sprite_shake);
    v0 = (s16)(v0 - shake);
    v1 = (s16)(v1 - shake);
    APTR d = sprite_data(n) + 2u * (u32)w;
    wr16(d, spr_pos_word(h, v0, v1, att));
    wr16(d + 2, spr_ctl_word(h, v0, v1, att));
}

/* 0x1F0DC: append a segment of v1 - v0 lines. */
static void sprite_add_segment(s16 h, s16 v0, s16 v1, s16 att, int n)
{
    u16 w = rd16(DADDR(D_sprite_next_word) + 2u * (u32)n);
    sprite_set_segment(w, h, v0, v1, att, n);
    wr16(DADDR(D_sprite_next_word) + 2u * (u32)n, (u16)(w + (u16)((v1 - v0) * 2 + 2)));
}

/* 0x1F12C: copy the wanted planes of a shape into a segment's DATA/DATB words, ANDed with the mask's plane 0
 * when one is given. `col`/`mcol` are word columns; the mask is read with the *shape's* height. */
static void pack_shape_to_sprite(APTR src, APTR dst, u16 planes, u16 col, APTR mask, u16 mcol)
{
    u16 ww = (u16)((rd16(src) + 1) >> 1);
    u16 mw = (u16)((rd16(mask) + 1) >> 1);              /* mask 0 reads the zero word at address 0, as in 68000 */
    u16 k = 0;
    APTR sp = src + 0x10, mp = mask + 0x10;
    u16 h = rd16(src + 2), np = rd16(src + 0xC);
    for (u16 p = 0; p < np; p++) {
        if ((1u << p) & planes) {
            u16 d = k, c = col, m = mcol;
            for (u16 r = 0; r < h; r++, d = (u16)(d + 2), c = (u16)(c + ww), m = (u16)(m + mw)) {
                u16 v = rd16(sp + 2u * (u32)c);
                if (mask) v &= rd16(mp + 2u * (u32)m);
                wr16(dst + 2u * (u32)d, v);
            }
            k++;
        }
        sp += 2u * (u32)h * (u32)ww;
    }
}

/* 0x1F27E: pack a shape into the next free segment of sprite n. */
static void shape_to_sprite_segment(APTR s, int n, u16 planes, u16 col)
{
    u16 w = rd16(DADDR(D_sprite_next_word) + 2u * (u32)n);
    pack_shape_to_sprite(s, sprite_data(n) + 2u * (u32)w + 4, planes, col, 0, 0);
    wr16(DADDR(D_sprite_next_word) + 2u * (u32)n, (u16)(w + (u16)(rd16(s + 2) * 2 + 2)));
}

/* 0x1F582: sprite 7 segment 0, the 4x4 steering-wheel marker dot. */
static void position_wheel_marker(s16 x, s16 y)
{
    sprite_set_segment(D16(D_dot_seg), (s16)(x + 0x80), (s16)(y + 0x2C), (s16)(y + 0x30), 0, 7);
}

/* 0x1CDFE: the four gear-knob sprites at (x, y) = (D:24B8, D:24B4). Sprites 4/5 are the left 16 pixels
 * (attached pair), 6/7 the right ones. */
static void knob_sprites_place(s16 x, s16 y)
{
    s16 v1 = (s16)(y + (s16)rd16(D32(S_gnob) + 2));              /* + the shape's height */
    sprite_set_segment(rd16(DADDR(D_knob_seg) + 0), x, y, v1, 0, 4);
    sprite_set_segment(rd16(DADDR(D_knob_seg) + 2), x, y, v1, 1, 5);
    sprite_set_segment(rd16(DADDR(D_knob_seg) + 4), (s16)(x + 0x10), y, v1, 0, 6);
    sprite_set_segment(rd16(DADDR(D_knob_seg) + 6), (s16)(x + 0x10), y, v1, 1, 7);
}

/* 0x1CF06: the same four sprites parked below the screen (v0 = 0x12C). */
static void knob_sprites_park(void)
{
    for (int n = 4; n < 8; n++) sprite_set_segment(rd16(DADDR(D_knob_seg) + 2u * (u32)(n - 4)), 0, 0x12C, 0, 0, n);
}

/* 0x1F5BA */
void sprites_init(void)
{
    SETD16(D_speed_pair_shift, 0);
    SETD16(D_tach_pair_shift, 0);
    APTR block = alloc_chip(0x7D0);
    for (int n = 0; n < 8; n++) {
        SETD32(D_sprite_data + 4 * n, block + 250u * (u32)n);
        wr16(DADDR(D_sprite_next_word) + 2u * (u32)n, 0);
    }
    SETD16(D_needle_seg, 0);
    if (D16(D_needle_gauges)) {
        s16 tx = DS16(D_tach_box_x), ty = DS16(D_tach_box_y);
        s16 sx = DS16(D_speed_box_x), sy = DS16(D_speed_box_y);
        sprite_add_segment((s16)(tx + 0x80), (s16)(ty + 0x2C), (s16)(ty + 0x5F), 0, 0);
        sprite_add_segment((s16)(tx + 0x90), (s16)(ty + 0x2C), (s16)(ty + 0x5F), 0, 1);
        sprite_add_segment((s16)(sx + 0x80), (s16)(sy + 0x2C), (s16)(sy + 0x5F), 0, 2);
        sprite_add_segment((s16)(sx + 0x90), (s16)(sy + 0x2C), (s16)(sy + 0x5F), 0, 3);
    }
    SETD16(D_dot_seg, rd16(DADDR(D_sprite_next_word) + 2 * 7));
    u16 dot = D16(D_dot_seg);
    for (int n = 4; n < 8; n++) {
        sprite_add_segment(0xE4, 0xA4, 0xA8, 0, n);
        APTR d = sprite_data(n) + 2u * (u32)dot + 4;
        static const u16 marker[8] = { 0x6000, 0x6000, 0xF000, 0xF000, 0xF000, 0xF000, 0x6000, 0x6000 };
        for (int i = 0; i < 8; i++) wr16(d + 2u * (u32)i, n == 7 ? marker[i] : 0);
    }
    for (int n = 4; n < 8; n++)
        wr16(DADDR(D_knob_seg) + 2u * (u32)(n - 4), rd16(DADDR(D_sprite_next_word) + 2u * (u32)n));
    knob_sprites_show = knob_sprites_place;                      /* drive_sim's knob_update draws through these */
    knob_sprites_hide = knob_sprites_park;
    APTR gnob = D32(S_gnob);
    shape_to_sprite_segment(gnob, 4, 0x3, 0);
    shape_to_sprite_segment(gnob, 5, 0xC, 0);
    shape_to_sprite_segment(gnob, 6, 0x3, 1);
    shape_to_sprite_segment(gnob, 7, 0xC, 1);
    if (D16(D_needle_gauges)) {
        APTR bm = alloc_public(0x28);
        SETD32(D_needle_bm, bm);
        gfx_InitBitMap(bm, 1, 0x20, 0x33);
        APTR rp = alloc_public(RP_SIZE);
        SETD32(D_needle_rp, rp);
        gfx_InitRastPort(rp);
        wr32(rp + RP_BitMap, bm);
        APTR sh = alloc_chip(0xDC);
        SETD32(D_needle_shape, sh);
        wr32(bm + BM_Planes, sh + 0x10);
        wr16(sh, 4);                                    /* width bytes */
        wr16(sh + 2, 0x33);                             /* height */
        wr16(sh + 0xC, 1);                              /* planes */
        wr16(sh + 0xE, 0xCC);                           /* plane bytes */
        gfx_SetAPen(rp, 1);
    }
    gfx_dmacon(0x8020);                                 /* sprite DMA on */
}

/* 0x1F99A */
void sprites_free(void)
{
    gfx_WaitTOF();
    gfx_dmacon(0x0020);
    if (D32(D_sprite_data)) free_mem(D32(D_sprite_data));
    SETD32(D_sprite_data, 0);
    if (D32(D_needle_shape)) free_mem(D32(D_needle_shape));
    SETD32(D_needle_shape, 0);
    if (D32(D_needle_rp)) free_mem(D32(D_needle_rp));
    SETD32(D_needle_rp, 0);
    if (D32(D_needle_bm)) free_mem(D32(D_needle_bm));
    SETD32(D_needle_bm, 0);
}

/* ================================================================ copper lists and palettes (§4.9) */

/* 0x1EF14: a long into a register pair (hi, then lo at reg + 2). */
static void cop_move_long(APTR ucl, u32 reg, u32 value)
{
    gfx_CMove(ucl, reg, (u16)(value >> 16));
    gfx_CBump(ucl);
    gfx_CMove(ucl, reg + 2, (u16)value);
    gfx_CBump(ucl);
}

/* 0x1EFD4 */
static void cop_sprite_pos(APTR ucl, s16 h, s16 v0, s16 v1, s16 att, int n)
{
    gfx_CMove(ucl, (u32)(CUSTOM_SPR0POS + 8 * n), spr_pos_word(h, v0, v1, att));
    gfx_CBump(ucl);
    gfx_CMove(ucl, (u32)(CUSTOM_SPR0CTL + 8 * n), spr_ctl_word(h, v0, v1, att));
    gfx_CBump(ucl);
}

/* 0x1FAD4: the sprite pointers, ten lines above the display. */
void ucop_sprite_pointers(APTR ucl)
{
    gfx_CWait(ucl, -10, 0);
    gfx_CBump(ucl);
    for (int n = 0; n < 8; n++) {
        cop_sprite_pos(ucl, 0x80, 0x28, 0x28, 0, n);
        cop_move_long(ucl, (u32)(CUSTOM_SPR0PT + 4 * n), sprite_data(n));
    }
}

/* 0x1FB4A: the 32 colour registers reloaded at `line`. */
void ucop_palette_split(APTR ucl, APTR pal, s16 line)
{
    gfx_CWait(ucl, (s16)(line - 1), (s16)D16(D_palette_split_h));
    gfx_CBump(ucl);
    for (int i = 0; i < 32; i++) {
        gfx_CMove(ucl, (u32)(CUSTOM_COLOR00 + 2 * i), rd16(pal + 2u * (u32)i));
        gfx_CBump(ucl);
    }
    gfx_CWait(ucl, 10000, 255);
    gfx_CBump(ucl);
}

/* 0x24D68 */
void remake_view(APTR v)
{
    gfx_MakeVPort(v, rd32(v + VIEW_ViewPort));
    gfx_MrgCop(v);
}

/* 0x24D8A: capture the dashboard palette once, build the drive's user copper list (sprite pointers + palette
 * split at 0x75) for this View and load the road palette into its ColorMap. */
void setup_drive_copper(APTR v)
{
    APTR vp = rd32(v + VIEW_ViewPort);
    APTR pal;
    if (D16(D_dash_palette_valid)) {
        pal = DADDR(D_dash_palette);
    } else {
        pal = rd32(rd32(vp + VP_ColorMap) + CM_ColorTable);
        for (int i = 0; i < 0x20; i++) wr16(DADDR(D_dash_palette) + 2u * (u32)i, rd16(pal + 2u * (u32)i));
    }
    SETD16(D_dash_palette_valid, 1);
    APTR ucl = exec_AllocMem(UCL_SIZE, MEMF_PUBLIC | MEMF_CLEAR);
    if (!ucl) ahost_fatal("out of memory (drive copper list)");
    ucop_sprite_pointers(ucl);
    ucop_palette_split(ucl, pal, 0x75);
    wr32(vp + VP_UCopIns, ucl);
    remake_view(v);                                     /* the snapshot keeps the merged list */
    gfx_LoadRGB4(vp, DADDR(D_road_palette), 0x20);
    gfx_FreeVPortCopLists(rd32(DADDR(D_g_viewA) + VIEW_ViewPort));
    gfx_FreeVPortCopLists(rd32(DADDR(D_g_viewB) + VIEW_ViewPort));
}

/* game/scene.h: the display side of a stage, for the drive flow (0x24ACA's body around the file loads). */
void dash_views_setup(void) { setup_drive_copper(D32(D_g_backView)); }

static s16 min16(s16 a, s16 b) { return a < b ? a : b; }        /* 0x1FE38 */

/* 0x1FE54: every 4-bit gun capped at `level`. */
static u16 clamp_rgb4(u16 c, s16 level)
{
    return (u16)(min16((s16)(c & 0xF00), (s16)(level << 8))
               + min16((s16)(c & 0x0F0), (s16)(level << 4))
               + min16((s16)(c & 0x00F), level));
}

/* 0x1FEAE: fade the shown View to black in 16 steps; with pal2 the lower part of the split fades too. */
void fade_out_palette(s16 delay, APTR pal2, s16 line)
{
    APTR front = D32(D_g_frontView), vp = rd32(front + VIEW_ViewPort);
    gfx_FreeVPortCopLists(vp);
    u16 a[32], b[32];
    APTR table = rd32(rd32(vp + VP_ColorMap) + CM_ColorTable);
    for (int i = 0; i < 32; i++) {
        a[i] = rd16(table + 2u * (u32)i);
        b[i] = pal2 ? rd16(pal2 + 2u * (u32)i) : 0;
    }
    for (s16 lv = 15; lv >= 0; lv--) {
        for (int i = 0; i < 32; i++) {
            a[i] = clamp_rgb4(a[i], lv);
            b[i] = clamp_rgb4(b[i], lv);
        }
        if (pal2) {
            gfx_FreeVPortCopLists(vp);
            APTR ucl = exec_AllocMem(UCL_SIZE, MEMF_PUBLIC | MEMF_CLEAR);
            if (!ucl) ahost_fatal("out of memory (fade copper list)");
            gfx_CWait(ucl, (s16)(line - 1), 100);
            gfx_CBump(ucl);
            for (int i = 0; i < 32; i++) {
                gfx_CMove(ucl, (u32)(CUSTOM_COLOR00 + 2 * i), b[i]);
                gfx_CBump(ucl);
            }
            gfx_CWait(ucl, 10000, 255);
            gfx_CBump(ucl);
            wr32(vp + VP_UCopIns, ucl);
            remake_view(front);
        }
        for (int i = 0; i < 32; i++) wr16(table + 2u * (u32)i, a[i]);
        dos_Delay((u32)(u16)delay);
    }
    gfx_FreeVPortCopLists(vp);
}

/* 0x24E52 */
void fade_out_drive(void)
{
    fade_out_palette(1, DADDR(D_dash_palette), 0x75);
    gfx_FreeVPortCopLists(rd32(DADDR(D_g_viewA) + VIEW_ViewPort));
    gfx_FreeVPortCopLists(rd32(DADDR(D_g_viewB) + VIEW_ViewPort));
    remake_view(DADDR(D_g_viewA));
    remake_view(DADDR(D_g_viewB));
}

/* ================================================================ the object queue (§4.5) */

/* 0x20410: push (x, y clamped to the crest, code, scale); at most 199 entries. */
static void queue_object(s16 x, s16 y, u16 code)
{
    s16 n = DS16(D_queue_count);
    if (n >= 0xC7) return;
    u32 o = 2u * (u32)(u16)n;
    wr16(DADDR(D_queue_x) + o, (u16)x);
    if (y >= DS16(D_crest_y)) y = DS16(D_crest_y);
    wr16(DADDR(D_queue_y) + o, (u16)y);
    wr16(DADDR(D_queue_code) + o, code);
    wr16(DADDR(D_queue_scale) + o, D16(D_row_scale));
    SETD16(D_queue_count, n + 1);
}

/* 0x205A6 -> byte offsets 0, 4, 8, 12, 16 */
static u16 scale_index5(u16 s)
{
    s = (u16)(s + 0x1000);
    if (s & 0x8000) s = 0x7FFF;
    return (u16)(((u32)s * 0x28u) >> 16) & 0x1C;
}

/* the whole register 0x205A6 leaves behind (low word = the index, high word = the product's low half) */
static u32 scale_index5_reg(u16 s)
{
    s = (u16)(s + 0x1000);
    if (s & 0x8000) s = 0x7FFF;
    u32 p = (u32)s * 0x28u;
    return (p << 16) | (u32)(((p >> 16) & 0x1C));
}

/* 0x205BC -> 0, 4, 8, 12 */
static u16 scale_index4(u16 s)
{
    s = (u16)(s + 0x1000);
    if (s & 0x8000) s = 0x7FFF;
    return (u16)((s >> 11) & 0x0C);
}

/* 0x2034E: the colour-6 block right of the cliff line and `clf0` on it. */
static void draw_horizon_cliff(void)
{
    s16 x = (s16)((DS16(D_cliff_x) + 0x20) & 0xFFF0);
    if (x < 0) x = 0;
    s16 wb = (s16)((s16)((s16)(DS16(D_clip_right) - x) >> 3) & 0xFFFE);
    if (wb > 0) {
        s16 h = DS16(D_cliff_y);
        if (h > DS16(D_window_bottom)) { h = DS16(D_window_bottom); SETD16(D_cliff_y, h); }
        h = (s16)(h - DS16(D_clip_top));
        if (h <= 0) return;
        s16 top = DS16(D_clip_top);
        blit_clear_rect(dest_plane(0), x, top, wb, h);
        blit_fill_rect(dest_plane(1), x, top, wb, h);
        blit_fill_rect(dest_plane(2), x, top, wb, h);
        blit_clear_rect(dest_plane(3), x, top, wb, h);
        blit_clear_rect(dest_plane(4), x, top, wb, h);
    }
    s16 save = DS16(D_clip_bottom);
    SETD16(D_clip_bottom, DS16(D_cliff_y));
    blit_hot(D32(S_clf0), D32(S_clf0_M), DS16(D_cliff_x), DS16(D_cliff_y));
    SETD16(D_clip_bottom, save);
}

/* 0x20526 / 0x20546 / 0x20562 / 0x20580: the roadside scenery and the posts. */
static void draw_obj_lin(s16 x, s16 y, u16 s)
{
    s16 i = (s16)scale_index4(s);
    blit_hot(tbl(S_lin, i), tbl(S_lin_M, i), (s16)(x + 6), y);
}

static void draw_obj_rock(s16 x, s16 y, u16 s)
{
    s16 i = (s16)scale_index4(s);
    blit_hot(tbl(S_rck, i), tbl(S_rck_M, i), x, y);
}

static void draw_obj_wed(s16 x, s16 y, u16 s)
{
    s16 i = (s16)scale_index4(s);
    blit_hot(tbl(S_wed, i), tbl(S_wed_M, i), (s16)(x + 6), y);
}

static void draw_obj_post(s16 x, s16 y, u16 s)
{
    y = (s16)(y - (s16)(((u32)0x23u * s) >> 16));
    s16 i = (s16)scale_index4(s);
    blit_hot(tbl(S_pst, i), tbl(S_pst_M, i), x, y);
}

/* 0x20740 */
static void draw_obj_hazard(s16 x, s16 y, u16 code, u16 s)
{
    s16 i = (s16)scale_index5(s);
    s16 c = (s16)((code - 0x20) * 4);
    APTR img = tbl(D_hazard_table, c), msk = tbl(D_hazard_mask_table, c);
    blit_hot(img ? rd32(img + (u32)(s32)i) : 0, msk ? rd32(msk + (u32)(s32)i) : 0, x, y);
}

/* 0x206A0: the mirror shows the back of a sign on its pole, in two sizes only. */
static void draw_obj_sign_mirror(s16 x, s16 y, u16 s)
{
    s16 i = (s16)scale_index4(s);
    if (i > 4) i = 4;
    blit_hot(tbl(S_pol, i), tbl(S_pol_M, i), x, y);
    blit_hot(tbl(S_rsg, i), tbl(S_rsg_M, i), x, y);
}

/* 0x206E8: codes 2..0xF, a pole and the sign face. */
static void draw_obj_sign(s16 x, s16 y, u16 code, u16 s)
{
    y = (s16)(y - (s16)(((u32)0x5Au * s) >> 16));
    if (DS16(D_mirror_flag)) { draw_obj_sign_mirror(x, y, s); return; }
    s16 i = (s16)scale_index5(s);
    blit_hot(tbl(S_pol, i), tbl(S_pol_M, i), x, y);
    s16 c = (s16)(code * 4);
    APTR img = tbl(D_sign_table, c), msk = tbl(D_sign_mask_table, c);
    blit_hot(img ? rd32(img + (u32)(s32)i) : 0, msk ? rd32(msk + (u32)(s32)i) : 0, x, y);
}

/* 0x205CE: traffic and police, with the per-code size hysteresis (x0.828 growing / x1.207 shrinking). */
static void draw_obj_traffic(s16 x, s16 y, u16 code, u16 s)
{
    SETD16(D_clip_bottom, DS16(D_window_bottom));
    s16 d2 = (s16)((code - 0x10) * 4);
    SETD16(D_traffic_scale_tmp, s);
    u32 r = scale_index5_reg(s);
    u16 have = rd16(DADDR(D_traffic_size) + (u32)(s32)d2);
    if ((u16)r != have) {
        if ((u16)r > have) {
            r = scale_index5_reg((u16)(((u32)s * 0xD3FCu) >> 16));
        } else {
            u32 n = ((u32)s << 16) | (r >> 16);         /* the low half of the previous product */
            if ((n / 0xD3FCu) <= 0xFFFFu) r = scale_index5_reg((u16)(n / 0xD3FCu));
            else                          r = scale_index5_reg((u16)n);   /* divu overflow: d4 unchanged */
        }
    }
    wr16(DADDR(D_traffic_size) + (u32)(s32)d2, (u16)r);
    s16 i = (s16)(u16)r;
    bool mir = DS16(D_mirror_flag) != 0;
    APTR imgs = tbl(mir ? D_mirror_car_table : D_car_table, d2);
    APTR msks = tbl(mir ? D_mirror_car_mask_table : D_car_mask_table, d2);
    blit_hot(imgs ? rd32(imgs + (u32)(s32)i) : 0, msks ? rd32(msks + (u32)(s32)i) : 0, x, y);
    if (d2 != 0x34) return;
    SETD8(D_police_flash, D8(D_police_flash) + 1);
    if (D8(D_police_flash) & 2) {
        APTR bar = tbl(mir ? S_cpl : S_clr, i);
        blit_shape_xor(bar, (s16)(x - shape_hot_x(bar)), (s16)(y - shape_hot_y(bar)));
    }
    if (D16(D_cop_mode) == 0xF) {
        APTR b = tbl(S_cpb, i);
        if (b) blit_shape_xor(b, (s16)(x - shape_hot_x(b)), (s16)(y - shape_hot_y(b)));
    }
}

/* 0x20456: pop the queue LIFO, each object clipped at its own base line. The cliff block is drawn just before
 * the first object nearer than the cliff row. */
static void draw_object_queue(void)
{
    s16 save = DS16(D_clip_bottom);
    for (;;) {
        s16 n = (s16)(DS16(D_queue_count) - 1);
        SETD16(D_queue_count, n);
        if (n < 0) break;
        u32 o = 2u * (u32)(u16)n;
        s16 x = (s16)rd16(DADDR(D_queue_x) + o);
        s16 y = (s16)rd16(DADDR(D_queue_y) + o);
        SETD16(D_clip_bottom, y);
        u16 code = rd16(DADDR(D_queue_code) + o);
        u16 s = rd16(DADDR(D_queue_scale) + o);
        if (s > D16(D_cliff_scale)) {                   /* unsigned */
            draw_horizon_cliff();
            SETD16(D_cliff_scale, 0xFFFF);
        }
        if (code >= 2 && code <= 0x0F)            draw_obj_sign(x, y, code, s);
        else if (code >= 0x10 && code <= 0x1D)    draw_obj_traffic(x, y, code, s);
        else if (code >= 0x20 && code <= 0x23)    draw_obj_hazard(x, y, code, s);
        else if (code == 0x30)                    draw_obj_wed(x, y, s);
        else if (code == 0x31)                    draw_obj_rock(x, y, s);
        else if (code == 0x32)                    draw_obj_lin(x, y, s);
        else if (code == 0x41)                    draw_obj_post(x, y, s);
    }
    SETD16(D_clip_bottom, save);
    SETD16(D_queue_count, 0);
    if (D16(D_cliff_scale) != 0xFFFF) draw_horizon_cliff();
}

/* ================================================================ road fill (§4.4) */

/* 0x20096: one row of a single plane between x0 and x1 (inclusive), masked at both ends.
 * README bug 5: the original blits D = A, so the bits outside the span in the end words become 0. */
static void fill_row_span(APTR plane, u16 off, s16 x0, s16 x1)
{
    s16 lo = DS16(D_clip_left), hi = DS16(D_clip_right);
    if (x0 > hi) return;
    if (x0 < lo) x0 = lo;
    if (x1 < lo) return;
    if (x1 > hi) x1 = hi;
    u16 b0 = (u16)(((u16)x0 >> 3) & 0xFFFE);
    u16 b1 = (u16)((((u16)x1 >> 3) & 0xFFFE) + 2);
    s16 bytes = (s16)(b1 - b0);
    if (bytes <= 0) return;                             /* x1 < x0: the original would blit a garbage size */
    APTR a = plane + (u32)(s32)(s16)off + b0;
    u16 fwm = rd16(DADDR(D_first_mask) + 2u * (u32)(x0 & 0xF));
    u16 lwm = rd16(DADDR(D_last_mask) + 2u * (u32)((x1 & 0xF) + 1));
    int words = bytes / 2;
    blit_wait();
    for (int k = 0; k < words; k++) {
        u16 v = 0xFFFF;
        if (k == 0) v &= fwm;
        if (k == words - 1) v &= lwm;
        APTR p = a + 2u * (u32)k;
        wr16(p, g_original_bugs ? v : (u16)(rd16(p) | v));
    }
}

/* 0x20128: the trapezoid between the previous row and this one — road (planes 0 + 2 + 3 = 13), shoulders
 * (2 + 3 = 12) and the right-hand ground (1 + 2 = 6) over the colour-4 background. */
static void fill_road_row(void)
{
    SETD16(D_shoulder_plane4, 0);
    s16 sh = (s16)(((u32)0x6Eu * D16(D_row_scale)) >> 16);
    s16 y = (s16)(D32(D_row_y) >> 16);
    if (y < DS16(D_min_row_y)) SETD16(D_min_row_y, y);
    s16 cx = (s16)(DS16(D_right_edge) + sh);
    if (cx < DS16(D_cliff_x)) {
        SETD16(D_cliff_x, cx);
        SETD16(D_cliff_scale, D16(D_row_scale));
        SETD16(D_cliff_y, y);
        if (DS16(D_min_row_y) < DS16(D_cliff_y)) SETD16(D_cliff_y, DS16(D_min_row_y));
    }
    SETD16(D_clip_right, DS16(D_clip_right) - 1);
    if (y < DS16(D_crest_y)) {
        if (DS16(D_crest_y) < DS16(D_prev_y)) SETD16(D_prev_y, DS16(D_crest_y));
        s16 row = (s16)(DS16(D_prev_y) - 1);
        u16 off = (u16)((u16)row * 0x28u);
        s16 n = (s16)(row - y);
        if (n != 0) {
            s16 dl = (s16)(DS16(D_left_edge) - DS16(D_prev_left));
            s16 dr = (s16)(DS16(D_right_edge) - DS16(D_prev_right));
            /* README bug 6: with n == 1 the original computes the halves and drops them, so the trapezoid
             * keeps the previous call's step. */
            if (n != 1 || !g_original_bugs) {
                SETD16(D_step_left, trunc0(dl, (s16)(n + 1)));
                SETD16(D_step_right, trunc0(dr, (s16)(n + 1)));
            }
        }
        for (;;) {
            fill_row_span(dest_plane(0), off, DS16(D_prev_left), DS16(D_prev_right));
            s16 xl = (s16)(DS16(D_prev_left) - sh);
            wr16(DADDR(D_left_shoulder_x) + 2u * (u32)(u16)DS16(D_prev_y), (u16)xl);
            if (xl > DS16(D_max_left_x)) {
                SETD16(D_max_left_x, xl);
                SETD16(D_max_left_row, DS16(D_prev_y));
            }
            fill_row_span(dest_plane(D16(D_shoulder_plane4) ? 4 : 3), off, xl, (s16)(DS16(D_prev_right) + sh));
            fill_row_span(dest_plane(1), off, (s16)(DS16(D_prev_right) + sh + 1), DS16(D_clip_right));
            SETD16(D_prev_y, DS16(D_prev_y) - 1);
            if (DS16(D_prev_y) <= y) break;
            SETD16(D_prev_left, DS16(D_prev_left) + DS16(D_step_left));
            off = (u16)(off - 0x28);
            SETD16(D_prev_right, DS16(D_prev_right) + DS16(D_step_right));
        }
    }
    SETD16(D_clip_right, DS16(D_clip_right) + 1);
    SETD16(D_prev_left, DS16(D_left_edge));
    SETD16(D_prev_right, DS16(D_right_edge));
    SETD16(D_prev_y, y);
}

/* 0x202A8: colour 6 in the left-hand dips, below the row where the left shoulder comes back furthest left. */
static void fill_left_ground(void)
{
    s16 top = DS16(D_prev_y), best = DS16(D_max_left_x), brow = DS16(D_prev_y);
    for (s16 r = DS16(D_max_left_row); r > top; r--) {
        s16 v = (s16)rd16(DADDR(D_left_shoulder_x) + 2u * (u32)(u16)r);
        if (v < best) { best = v; brow = r; }
    }
    if (best >= DS16(D_max_left_x)) return;
    /* scanline brow keeps row brow + 1's shoulder x, as the original's off-by-one indexing does */
    u16 off = (u16)((u16)brow * 0x28u);
    s16 r = (s16)(brow + 1);
    do {
        s16 x1 = (s16)(rd16(DADDR(D_left_shoulder_x) + 2u * (u32)(u16)r) - 1);
        if (x1 >= best) fill_row_span(dest_plane(1), off, best, x1);
        r++;
        off = (u16)(off + 0x28);
    } while (r <= DS16(D_window_bottom));
}

/* 0x2031C: the 2-pixel yellow centre line, dashed by bit 2 of the unit unless the road is wide. */
static void draw_centre_line(s16 unit)
{
    if (!DS16(D_wide_road) && !(unit & 4)) return;
    s16 x = (s16)(D32(D_row_cx) >> 16), y = (s16)(D32(D_row_y) >> 16);
    SETD16(D_pixel_plane_off, 0x0C);
    blit_wait();
    plot_pixel_clear(x, y);
    plot_pixel_clear((s16)(x + 1), y);
}

/* 0x1E65C: white lane marks on wide roads (right half > 0x224). */
static void draw_lane_marks(s16 unit)
{
    SETD16(D_wide_road, 0);
    if ((s16)D16(D_half_right) <= 0x224) return;
    SETD16(D_wide_road, -1);
    if (!(unit & 4)) return;
    s16 x = (s16)(DS16(D_row_centre) + (s16)(((u32)0x1C0u * D16(D_row_scale)) >> 16));
    s16 y = (s16)(D32(D_row_y) >> 16);
    SETD16(D_pixel_plane_off, 0x10);
    plot_pixel_set(x, y);
    SETD16(D_pixel_plane_off, 0x00);
    plot_pixel_clear(x, y);
}

/* ================================================================ the row walker (§4.3) */

/* 0x1E5C6: the row's half-widths from the record's first byte (hi nibble left, lo nibble right). */
static void row_widths(APTR rec)
{
    static const u16 W[16] = { 0x140, 0x180, 0x1C0, 0x200, 0x240, 0x280, 0x2C0, 0x300,
                               0x340, 0x380, 0x3C0, 0x400, 0x440, 0x480, 0x4C0, 0x500 };
    u8 b = rd8(rec);
    if (b == 0xFF) {
        if (D16(D_row_index) == 0) {
            SETD16(D_half_left, D16(D_car_half_left));
            SETD16(D_half_right, D16(D_car_half_right));
        } else {
            SETD16(D_half_left, D16(D_last_left));
            SETD16(D_half_right, D16(D_last_right));
        }
    } else {
        SETD16(D_half_left, W[(b >> 4) & 0xF]);
        SETD16(D_half_right, W[b & 0xF]);
        if (D16(D_row_index) == 0) {
            SETD16(D_car_half_left, D16(D_half_left));
            SETD16(D_car_half_right, D16(D_half_right));
        }
    }
    SETD16(D_last_left, D16(D_half_left));
    SETD16(D_last_right, D16(D_half_right));
}

/* game/scene.h: the row-0 widths the simulation reads (D:0D94/D:0D96) even when nothing is drawn. */
void row_road_widths(void)
{
    u16 saved = D16(D_row_index);
    SETD16(D_row_index, 0);
    row_widths(road_record_at((s16)(D32(D_road_pos) >> 16)));
    SETD16(D_row_index, saved);
}

/* 0x1DD34 */
static void row_scale_step(void)
{
    SETD16(D_row_scale, (u16)(((u32)D16(D_row_scale) * 0xE8F5u) >> 16));
    SETD16(D_row_scale2, (u16)(((u32)D16(D_row_scale2) * 0xE8F5u) >> 16));
    SETD16(D_first_row_weight, 0);
}

/* 0x1DD56: crest update, then y -= 8s + slope*s/4. */
static void row_y_step(void)
{
    s16 y = (s16)(D32(D_row_y) >> 16);
    if (y <= DS16(D_crest_y)) SETD16(D_crest_y, y);
    u32 d1 = 8u * (u32)D16(D_row_scale);
    u16 w = D16(D_first_row_weight);
    if (w) d1 = ((u32)(u16)(d1 >> 8) * (u32)w) >> 7;
    s16 sl = DS16(D_slope_acc);
    if (DS16(D_mirror_flag)) sl = (s16)(-sl);
    s32 d0 = (mul_scale(sl) >> 2) + (s32)d1;
    SETD32(D_row_y, (u32)(D32(D_row_y) - (u32)d0));
}

/* 0x2472A: x += sin(heading) * 80 * s, plus the lateral parallax. The main view re-seeds the heading from the
 * car's until the rows reach y < 0x7D (plus one more row), so rows below the window never bend. */
static void row_x_step(void)
{
    u16 k = 0x50;
    if (DS16(D_mirror_flag)) {
        k = 0x28;
    } else {
        if ((s16)(D32(D_row_y) >> 16) >= 0x7D) {
            SETD16(D_near_rows_flag, -1);
            SETD32(D_heading_acc, D32(D_view_yaw));
            return;
        }
        if (DS16(D_near_rows_flag)) {
            SETD16(D_near_rows_flag, 0);
            SETD32(D_heading_acc, D32(D_view_yaw));
            return;
        }
    }
    s32 h = DS32(D_heading_acc);
    bool neg = h < 0;
    if (neg) h = -h;
    u32 sn = (u32)sin_deg(h);
    s32 d = (s32)((u32)(u16)sn * (u32)k) >> 8;
    d = mul_scale((s16)d) >> 8;
    SETD32(D_row_cx, (u32)(DS32(D_row_cx) + (neg ? -d : d)));
    if (DS16(D_mirror_flag)) {
        s32 t = mul_scale((s16)(-(s16)DS16(D_r_lateral))) >> 3;
        SETD32(D_row_cx, (u32)(DS32(D_row_cx) - t));
    } else {
        s32 t = mul_scale((s16)DS16(D_r_lateral)) >> 2;
        SETD32(D_row_cx, (u32)(DS32(D_row_cx) + t - (t >> 2)));
    }
}

/* 0x246FA: heading += curve / 8, clamped to +-90 degrees. */
static void row_heading_step(APTR rec)
{
    s32 c = (s8)rd8(rec + 1);
    u16 w = D16(D_first_row_weight);
    c = w ? (((s32)(s16)c * (s32)(s16)w) << 1) : (c << 16);
    SETD32(D_heading_acc, (u32)(DS32(D_heading_acc) + (c >> 3)));
    clamp_word(DADDR(D_heading_acc), -0x5A, 0x5A);      /* the high word only */
}

/* 0x1DD96: slope += pitch * f(scale), clamped per row by D:0B38. */
static void row_slope_step(APTR rec)
{
    u32 d1 = D16(D_row_scale);
    u16 w = D16(D_first_row_weight);
    if (w) d1 = ((u32)D16(D_row_scale) * (u32)w << 1) >> 16;
    if (DS16(D_mirror_flag)) d1 <<= 1;
    s32 t = ((s32)0x48000 - (s32)(4u * d1)) >> 4;
    s32 p = (s32)(s16)(s8)rd8(rec + 2) * (s32)(s16)(u16)t;
    SETD16(D_slope_acc, (u16)(DS16(D_slope_acc) + (s16)(p >> 16)));
    s16 lim = (s8)rd8(DADDR(D_slope_clamp) + D16(D_row_index));
    clamp_word(DADDR(D_slope_acc), (s16)(-lim), lim);
}

/* ---- the queue functions of one row (0x1E062 - 0x1E2F8) */

/* 0x1E062 .. 0x1E0CC: the common tail at 0x1E124 / 0x1E142. `slot` 0..4 same direction, 5 = the police car. */
static void queue_slot_car(int slot, s16 lat, s16 y, s16 unit)
{
    u32 o = 4u * (u32)slot;
    if (unit != (s16)(rd32(DADDR(D_r_same_dir) + o) >> 16)) return;
    if (slot == 5) {
        if (D16(D_cop_mode) >= 10) {                    /* unsigned */
            u16 t = D16(D_cop_timer) > 0x3C ? 0x3C : D16(D_cop_timer);
            s16 a = (s16)(t - 0x1E);
            if (a < 0) a = (s16)(-a);
            lat = (s16)(((s16)(a - 0x1E) << 4) + 0xE0);
        } else {
            lat = 0xE0;
            s16 sl = DS16(D_police_slide);
            if (sl < 0) {
                sl = (s16)(sl + 0x38);
                if (sl < (s16)0xFF20) sl = (s16)0xFF20;
                lat = (s16)(lat + sl);
                SETD16(D_police_slide, sl);
            }
        }
    } else {
        s16 sl = (s16)rd16(DADDR(D_sd_dodge) + o);
        if (sl > 0) {
            sl = (s16)(sl - 0x38);
            if (sl > 0xE0) sl = 0xE0;
            lat = (s16)(lat + sl);
            wr16(DADDR(D_sd_dodge) + o, (u16)sl);
        }
    }
    wr16(DADDR(D_sd_lane) + o, (u16)(DS16(D_slot_base_lat) + lat));     /* read back by the simulation */
    u16 save = D16(D_row_scale);
    if (D16(D_row_index) <= 6) {
        if ((s16)D16(D_row_index) <= 2) return;         /* rows 0..2: not drawn at all */
        SETD16(D_row_scale, DS16(D_mirror_flag) ? 0x2D6E : 0x9160);
    }
    s32 p = (s32)lat * (s32)(s16)(D16(D_row_scale) >> 1);
    s16 dx = (s16)((s16)(p >> 16) << 1);
    queue_object((s16)(DS16(D_slot_base_x) + dx), y, (u16)(0x18 + slot));
    SETD16(D_row_scale, save);
}

/* 0x1E19E: the five oncoming slots, queued before the widths are known. */
static void queue_oncoming_cars(s16 x, s16 y, s16 unit)
{
    u16 save = D16(D_row_scale);
    if (D16(D_row_index) <= 6) {
        if ((s16)D16(D_row_index) <= 2) return;
        SETD16(D_row_scale, DS16(D_mirror_flag) ? 0x2D6E : 0x9160);
    }
    for (int k = 0; k < 5; k++) {
        if (unit != (s16)(rd32(DADDR(D_r_oncoming) + 4u * (u32)k) >> 16)) continue;
        wr16(DADDR(D_on_lane) + 4u * (u32)k, (u16)(DS16(D_slot_base_lat) - 0xE0));
        s16 d = (s16)(((u32)0xE0u * D16(D_row_scale)) >> 16);
        queue_object((s16)(x - d), y, (u16)(0x10 + k));
    }
    SETD16(D_row_scale, save);
}

/* 0x1E216: `pst` at the left road edge every 16 units. */
static void queue_post(s16 x, s16 y, s16 unit)
{
    if ((unit & 0xF) == 0 && (s16)D16(D_row_index) > 6) queue_object(x, y, 0x41);
}

/* 0x1E230 / 0x1E276 / 0x1E2B0: the three roadside scenery tables, on the right. */
static void queue_scenery(s16 x, s16 y, s16 unit, const u8 *table, u16 code)
{
    u16 a = table[unit & 0x3F];
    if (!a || y >= 0x7A) return;
    if (code == 0x31) {                                 /* rck: no lift, offset 0x28 */
        x = (s16)(x + (s16)(((u32)0x28u * D16(D_row_scale)) >> 16));
        queue_object(x, y, code);
        return;
    }
    u16 mul = (u16)(code == 0x30 ? a * 2u : a * 8u);
    s16 t = (s16)(((u32)mul * D16(D_row_scale)) >> 16);
    y = (s16)(y - t);
    x = (s16)(x + (s16)(t >> 3));
    x = (s16)(x + (s16)(((u32)(code == 0x30 ? 0x6Eu : 0xB4u) * D16(D_row_scale)) >> 16));
    queue_object(x, y, code);
}

/* 0x1E2F8: a road record's object byte 0x20..0x23, in one of four lanes right of the centre. */
static void queue_hazard(s16 x, s16 y, u16 obj)
{
    s16 d = (s16)(((u32)(obj & 0xFF) * D16(D_row_scale)) >> 16);
    queue_object((s16)(x + 2 * d), y, (u16)(obj & 0x3F));
}

/* the scenery tables at 0x1E47A / 0x1E4BA / 0x1E4FA */
static const u8 scenery_wed[64] = {
    180,0,0,0,0,0,0,110,0,0,0,0,0,217,0,0, 0,130,0,0,0,0,0,0,0,0,100,0,0,60,0,0,
    0,0,0,200,0,0,50,0,0,0,140,0,0,0,0,0, 180,0,0,0,80,0,0,0,0,255,0,0,143,0,0,0,
};
static const u8 scenery_rck[64] = {
    2,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, 0,1,0,0,0,0,0,0,0,0,1,0,0,0,0,0,
    0,0,0,0,0,0,1,0,0,0,0,0,0,0,0,2, 1,0,0,0,0,0,0,0,0,0,0,0,1,0,0,0,
};
static const u8 scenery_lin[64] = {
    129,0,17,0,160,33,0,195,0,0,18,0,128,242,0,0, 0,51,0,0,115,0,18,0,241,0,128,0,19,193,0,161,
    0,208,0,67,0,225,128,0,0,0,65,0,162,0,32,0, 131,0,34,0,193,0,64,0,113,179,0,0,51,0,242,0,
};

/* 0x1E314: one road row per unit, 40 forward (30 backwards in the mirror), until the crest reaches the top of
 * the clip window. */
static void walk_road_rows(s16 unit)
{
    SETD16(D_row_index, 0);
    SETD16(D_slope_acc, 0);
    s16 left = DS16(D_mirror_flag) ? 0x1D : 0x27;
    for (;;) {
        APTR rec = road_record_at(unit);
        s16 obj = (s16)(s8)rd8(rec + 3);
        s16 x = (s16)(D32(D_row_cx) >> 16), y = (s16)(D32(D_row_y) >> 16);
        SETD16(D_row_centre, x);
        SETD16(D_slot_base_x, x);
        SETD16(D_slot_base_lat, 0);
        queue_slot_car(5, 0, y, unit);
        queue_slot_car(1, 0xE0, y, unit);
        queue_oncoming_cars(x, y, unit);
        if ((obj & 0x3F) >= 0x20 && (obj & 0x3F) <= 0x23) queue_hazard(x, y, (u16)obj);
        row_widths(rec);
        x = (s16)(x - (s16)(((u32)D16(D_half_left) * D16(D_row_scale)) >> 16));
        SETD16(D_left_edge, x);
        if ((s8)obj >= (s8)0x82 && (s8)obj <= (s8)0x8F && (s16)D16(D_row_index) > 6)
            queue_object(x, y, (u16)(obj & 0x7F));      /* signs on the left */
        queue_post(x, y, unit);
        SETD16(D_slot_base_lat, D16(D_half_right));
        x = (s16)(DS16(D_row_centre) + (s16)(((u32)D16(D_half_right) * D16(D_row_scale)) >> 16));
        SETD16(D_right_edge, x);
        SETD16(D_slot_base_x, x);
        if ((s16)D16(D_row_index) <= 6) {
            u32 p = (u32)(DS16(D_mirror_flag) ? 0x2D6Eu : 0x9160u) * (u32)D16(D_half_right);
            /* README bug 2: the original uses the product's low word (the `swap` every other mulu of the
             * walker has is missing here), so slots 0/2/3/4 land far off screen in rows 3..6. */
            u16 part = g_original_bugs ? (u16)p : (u16)(p >> 16);
            SETD16(D_slot_base_x, (s16)(part + (u16)DS16(D_row_centre)));
        }
        if ((u8)obj >= 2 && (u8)obj <= 0x0F && (s16)D16(D_row_index) > 6)
            queue_object(x, y, (u8)obj);                /* signs on the right */
        queue_scenery(x, y, unit, scenery_wed, 0x30);
        queue_scenery(x, y, unit, scenery_rck, 0x31);
        queue_scenery(x, y, unit, scenery_lin, 0x32);
        queue_slot_car(0, (s16)0xFF20, y, unit);
        queue_slot_car(2, (s16)0xFF20, y, unit);
        queue_slot_car(3, (s16)0xFF20, y, unit);
        queue_slot_car(4, (s16)0xFF20, y, unit);
        fill_road_row();
        draw_lane_marks(unit);
        draw_centre_line(unit);
        row_y_step();
        row_x_step();
        row_heading_step(rec);
        row_slope_step(rec);
        row_scale_step();
        if (DS16(D_crest_y) <= DS16(D_clip_top)) return;
        SETD16(D_row_index, D16(D_row_index) + 1);
        unit = (s16)(unit + (DS16(D_mirror_flag) ? -1 : 1));
        if (--left < 0) return;
    }
}

/* 0x1D36C / 0x1DAF6 */
static void set_main_window_clip(void)
{
    SETD16(D_crest_y, 0x75);
    set_clip(0, 0x75, 0, 0x140);
    SETD16(D_window_bottom, DS16(D_clip_bottom));
}

static void set_mirror_window_clip(void)
{
    SETD16(D_crest_y, 0x2B);
    set_clip(0x1A, 0x2B, 0xF0, 0x140);
    SETD16(D_window_bottom, DS16(D_clip_bottom));
}

/* 0x1D97C: the clip rectangle down to the crest, in colour 4 (sky and the left-hand side). */
static void clear_window(void)
{
    s16 x = DS16(D_clip_left), y = DS16(D_clip_top);
    s16 wb = (s16)((u16)(DS16(D_clip_right) - x) >> 3);
    s16 h = (s16)(DS16(D_crest_y) - y);
    if (h <= 0) return;
    blit_clear_rect(dest_plane(0), x, y, wb, h);
    blit_clear_rect(dest_plane(1), x, y, wb, h);
    blit_fill_rect(dest_plane(2), x, y, wb, h);
    blit_clear_rect(dest_plane(3), x, y, wb, h);
    blit_clear_rect(dest_plane(4), x, y, wb, h);
}

/* 0x1DA72: the first row's scale and sub-unit weight from the fraction of the road position. */
static void init_row_scale(void)
{
    SETD16(D_row_scale, 0xFFFF);
    SETD16(D_row_scale2, 0xFFFF);
    u16 f = (u16)(-(u16)D16(D_r_road_pos + 2));         /* neg.w of the low word */
    u16 w = (u16)(f >> 1);
    if (w == 0) w = 1;
    SETD16(D_first_row_weight, w);
    SETD16(D_row_scale, (u16)(D16(D_row_scale) - (u16)(((u32)f * 0x170Bu) >> 16)));
}

/* 0x1DA14 */
static void draw_road_main(s16 unit)
{
    SETD16(D_cliff_x, 20000);
    SETD16(D_cliff_y, 20000);
    SETD16(D_min_row_y, 20000);
    SETD32(D_heading_acc, 0);
    SETD32(D_row_y, 0xA00000);
    SETD16(D_row_y, (u16)(D16(D_row_y) + (u16)D16(D_horizon_shake)));   /* add.w to the high word */
    init_row_scale();
    SETD32(D_row_cx, (u32)(0xA00000 - mul_scale((s16)DS16(D_r_lateral))));
    /* 0x24A02 (drive_sim) is a lone rts */
    SETD16(D_mirror_flag, 0);
    SETD16(D_max_left_x, (s16)0xD8F0);
    SETD16(D_prev_y, 0x74);
    walk_road_rows(unit);
    fill_left_ground();
}

/* 0x1DAA2 */
static void draw_road_mirror(s16 unit)
{
    SETD16(D_cliff_x, 20000);
    SETD16(D_cliff_y, 20000);
    SETD16(D_min_row_y, 20000);
    SETD32(D_heading_acc, 0);
    SETD32(D_row_y, 0x3E0000);
    SETD16(D_row_scale, 0x5000);
    s16 lat = (s16)DS16(D_r_lateral);
    s16 cx = (s16)(0x118 - (s16)((s16)(lat * 5) >> 4));
    SETD32(D_row_cx, (u32)((u32)(u16)cx << 16));
    set_mirror_window_clip();
    clear_window();
    SETD16(D_mirror_flag, -1);
    walk_road_rows(unit);
}

/* ================================================================ clouds and bugs (§4.6) */

/* 0x1EB5E */
static s16 wrap_cloud_x(s16 v)
{
    while (v < (s16)0xFE70) v = (s16)(v + 0x320);
    while (v > 0x190) v = (s16)(v - 0x320);
    return v;
}

/* 0x1EBA8: d0 - d1 wrapped to [-180, 180] */
static s16 wrap_angle_delta(s16 a, s16 b)
{
    s16 d = (s16)(a - b);
    while (d < -180) d = (s16)(d + 360);
    while (d > 180) d = (s16)(d - 360);
    return d;
}

static s32 cloud_x(int i) { return DS32(D_cloud_x + 4 * i); }
static void set_cloud_x(int i, s32 v) { SETD32(D_cloud_x + 4 * i, (u32)v); }

/* 0x1EAE0: clouds 0 and 1 move -5 * delta, clouds 2 and 3 -6 * delta, plus a per-frame drift. */
static void move_clouds(s16 delta)
{
    s32 v = (s32)((u32)(u16)delta << 16);
    for (int i = 0; i < 4; i++) set_cloud_x(i, cloud_x(i) - v);
    v >>= 1;
    for (int i = 0; i < 4; i++) set_cloud_x(i, cloud_x(i) - v);
    v >>= 1;
    set_cloud_x(0, cloud_x(0) + v);
    set_cloud_x(1, cloud_x(1) + v);
    static const s32 drift[4] = { 0xFA0, 0x1388, 0x1770, 0x1B58 };
    for (int i = 0; i < 4; i++) {
        set_cloud_x(i, cloud_x(i) + drift[i]);
        SETD16(D_cloud_x + 4 * i, (u16)wrap_cloud_x((s16)(cloud_x(i) >> 16)));
    }
}

/* 0x1EB78 */
void init_clouds(void)
{
    static const s16 start[4] = { 0x1E, 0x78, 0x32, 0x15E };
    SETD16(D_cloud_heading, 0);
    for (int i = 0; i < 4; i++) SETD16(D_cloud_x + 4 * i, (u16)start[i]);
    SETD16(0x0DB0, 0x78);                               /* only the dead code at 0x1EA9A reads these */
    SETD16(0x0DB4, 0x15E);
    SETD16(0x0DB8, (u16)-0xC8);
}

/* 0x1E784 */
static void blit_clouds(void)
{
    static const s16 dy[4] = { 0x11, 0x14, 0x23, 0x26 };
    for (int i = 0; i < 4; i++) {
        s16 x = (s16)DS16(D_cloud_x + 4 * i);
        s16 y = (s16)((s16)(D32(D_row_y) >> 16) - dy[i]);
        APTR img = D32(S_cld + 4 * i), msk = D32(S_cld_M + 4 * i);
        blit_hot(img, msk, x, y);
    }
}

/* 0x1E758: the clouds drift with the world heading; the clip bottom stays at the crest afterwards. */
static void draw_clouds(void)
{
    SETD16(D_clip_bottom, DS16(D_crest_y));
    s16 h = (s16)(D32(D_heading) >> 16);
    s16 d = wrap_angle_delta(h, DS16(D_cloud_heading));
    SETD16(D_cloud_heading, h);
    move_clouds((s16)(d << 2));
    blit_clouds();
}

/* 0x1E7D2: the bug list lives in the overlay's own code at 0x1E89A (count word, then 30 entries of 14 bytes:
 * x, y, image, mask, time to live). 0x1CCF6 (drive_sim) clears it at stage start. */
#define BUG_LIST  0x1E89A

static void draw_bugs(void)
{
    SETD16(D_clip_bottom, 0x75);
    APTR list = BUG_LIST;
    u16 count = rd16(list);
    if ((s16)count < 0x1E && (u16)(D32(D_speed) >> 16) >= 0x55 && (rand16() & 0x3FF) == 0) {
        APTR e = list + 2 + 0xEu * (u32)count;
        wr16(e, (u16)((rand16() & 0xFF) + 0x20));
        wr16(e + 2, (u16)(rand16() & 0x7F));
        wr32(e + 4, D32(S_bug1));
        wr32(e + 8, D32(S_bug1_M));
        wr16(e + 0xC, 5);
        wr16(list, (u16)(count + 1));
        count = (u16)(count + 1);
    }
    for (u16 i = 0; i < count; i++) {
        APTR e = list + 2 + 0xEu * (u32)i;
        blit_shape(rd32(e + 4), rd32(e + 8), (s16)rd16(e), (s16)rd16(e + 2));
        u16 t = rd16(e + 0xC);
        if (!t) continue;
        wr16(e + 2, (u16)(rd16(e + 2) - 2));
        wr16(e + 0xC, (u16)(t - 1));
        if (t >= 3) continue;
        if (t == 1) {                                   /* the splat */
            s16 r = (s16)(rand16() & 3);
            wr32(e + 4, D32(S_bugX + 4 * (u16)r));
            wr32(e + 8, D32(S_bugX_M + 4 * (u16)r));
        } else {
            wr32(e + 4, D32(S_bug1b));
            wr32(e + 8, D32(S_bug1b_M));
        }
    }
}

/* ================================================================ cockpit overlays (§4.7) */

/* 0x1EC94: the radar detector blinks 50 ticks lit / 8 dark, and beeps at each re-light when the level is > 0. */
void radar_lamp(void)
{
    s16 dt = (s16)(D32(D_tick_count) - D32(D_radar_time));
    if (D16(D_radar_dark)) {
        if (dt < 8) return;
        SETD16(D_radar_dark, 0);
        SETD32(D_radar_time, D32(D_tick_count));
        if ((s16)(D32(D_radar_zone_ctr) >> 16)) radar_beep();
        return;
    }
    s16 l = (s16)(D32(D_radar_zone_ctr) >> 16);
    if (l > 5) l = 5;
    APTR s = D32(S_rad + 4 * (u16)l);
    blit_shape_xor_word(s, shape_x(s), shape_y(s));
    if (dt >= (s16)D16(D_radar_blink)) {
        SETD16(D_radar_dark, (u16)-1);
        SETD32(D_radar_time, D32(D_tick_count));
    }
}

/* 0x1ED64: pose 2 is the one already in the dashboard picture. */
static void xor_wheel_pose(s16 p)
{
    if (p == 2) return;
    if (p > 2) p--;
    APTR s = D32(S_whl + 4 * (u16)p);
    blit_shape_xor_word(s, shape_x(s), shape_y(s));
}

/* 0x1ED06: five poses, XORed over the centred wheel, with one pose memory per buffer. */
static void draw_steering_wheel(void)
{
    s16 st = (s16)(D32(D_r_steer) >> 16);
    s16 p = (s16)((abs_w(st) + 4) >> 3);
    if (p >= 3) p = 2;
    if (st < 0) p = (s16)(-p);
    p = (s16)(p + 2);
    if (p != DS16(D_wheel_pose_here)) {
        s16 old = DS16(D_wheel_pose_here);
        if (old != 2 && old >= 0 && old < 5) xor_wheel_pose(old);
        SETD16(D_wheel_pose_here, p);
        xor_wheel_pose(p);
    }
    s16 t = DS16(D_wheel_pose_other);
    SETD16(D_wheel_pose_other, DS16(D_wheel_pose_here));
    SETD16(D_wheel_pose_here, t);
}

/* 0x1ED8A */
void reset_wheel_state(s16 keep)
{
    if (!keep) {
        SETD16(D_wheel_pose_other, 0xFE0C);
        SETD16(D_wheel_pose_here, 0xFE0C);
    } else {
        SETD16(D_wheel_pose_here, DS16(D_wheel_pose_other));
    }
    SETD32(D_marker_cache, 0x87654321);
}

/* 0x1EBC4 */
static void draw_cockpit_overlays(void)
{
    set_clip_full();
    static const u16 pair[4][2] = { { S_hood, S_hood_M }, { S_roof, S_roof_M },
                                    { S_post, S_post_M }, { S_mirr, S_mirr_M } };
    for (int i = 0; i < 4; i++) {
        APTR s = D32(pair[i][0]);
        blit_shape_word(s, D32(pair[i][1]), shape_x(s), shape_y(s));
    }
    radar_lamp();
    draw_steering_wheel();
}

/* 0x203E0: the gear gate is XOR-drawn per buffer. */
static void toggle_gear_box(void)
{
    u32 b = D16(D_back_index);
    if (rd8(DADDR(D_gearbox_show) + b) == rd8(DADDR(D_gearbox_shown) + b)) return;
    wr8(DADDR(D_gearbox_shown) + b, (u8)~rd8(DADDR(D_gearbox_shown) + b));
    APTR s = D32(S_gbox);
    blit_shape_xor_word(s, shape_x(s), shape_y(s));
}

/* 0x1E63A */
static void draw_ticket(void)
{
    if (DS16(D_ticket_frames) < 0x1E) return;
    blit_at_own(D32(S_tick), D32(S_tick_M));
}

/* ================================================================ gauges (§4.8) */

/* 0x1F2EA: draw one needle into the 32x51 bitmap with graphics Draw, then pack it into its two sprites, masked
 * by the wheel-rim overlay. `which` 0 = tachometer (sprites 0/1), 2 = speedometer (2/3). */
static void draw_needle(s16 x, s16 y, s16 which)
{
    APTR bm = D32(D_needle_bm), plane = rd32(bm + BM_Planes);
    s16 off = (s16)(x > 0x18 ? 16 : 0);
    APTR ov = which ? D32(D_speed_overlay) : D32(D_tach_overlay);
    for (int i = 0; i < 0x33; i++) wr32(plane + 4u * (u32)i, 0);
    if ((s16)(x - off) < 0 || (s16)(x - off) > 0x1F || y < 0 || y >= 0x33) return;
    APTR rp = D32(D_needle_rp);
    if (which == 0)
        gfx_Move(rp, (s16)(rd8(DADDR(D_pivot_tach_x)) - DS16(D_tach_box_x) - off),
                     (s16)(rd8(DADDR(D_pivot_tach_y)) - DS16(D_tach_box_y)));
    else
        gfx_Move(rp, (s16)(rd8(DADDR(D_pivot_speed_x)) - DS16(D_speed_box_x) - off),
                     (s16)(rd8(DADDR(D_pivot_speed_y)) - DS16(D_speed_box_y)));
    gfx_Draw(rp, (s16)(x - off), y);
    APTR needle = D32(D_needle_shape);
    u16 seg = D16(D_needle_seg);
    APTR d0 = sprite_data(which) + 2u * (u32)seg + 4;
    APTR d1 = sprite_data(which + 1) + 2u * (u32)seg + 4;
    if (off) {
        pack_shape_to_sprite(needle, d1, 1, 0, ov, 1);
        pack_shape_to_sprite(needle, d0, 1, 1, ov, 2);
    } else {
        pack_shape_to_sprite(needle, d0, 1, 0, ov, 0);
        pack_shape_to_sprite(needle, d1, 1, 1, ov, 1);
    }
    if (which != 0 && which != 2) return;
    u16 shift_g = which ? D_speed_pair_shift : D_tach_pair_shift;
    if ((u16)(off * 2) == D16(shift_g)) return;
    SETD16(shift_g, (u16)(off * 2));
    s16 bx = which ? DS16(D_speed_box_x) : DS16(D_tach_box_x);
    s16 by = which ? DS16(D_speed_box_y) : DS16(D_tach_box_y);
    sprite_set_segment(D16(D_needle_seg), (s16)(bx + (s16)D16(shift_g) + 0x80),
                       (s16)(by + 0x2C), (s16)(by + 0x5F), 0, which);
}

/* 0x20A88: up to three XOR digits at fixed top-left positions. */
static void draw_number(u16 v, u16 digits, APTR xy)
{
    if (v >= 100) blit_shape_xor(rd32(DADDR(digits) + 4u * (u32)(v / 100)),
                                 (s16)rd16(xy), (s16)rd16(xy + 2));
    if (v >= 10) blit_shape_xor(rd32(DADDR(digits) + 4u * (u32)((v / 10) % 10)),
                                (s16)rd16(xy + 4), (s16)rd16(xy + 6));
    blit_shape_xor(rd32(DADDR(digits) + 4u * (u32)(v % 10)), (s16)rd16(xy + 8), (s16)rd16(xy + 10));
}

/* 0x2097C: the Corvette's digital cluster — the instrument face, two bars revealed by the clip window and
 * XOR digits, with the wheel-rim overlays on top. */
static void draw_digital_cluster(void)
{
    gfx_OwnBlitter();
    set_clip_full();
    blit_begin();
    APTR s = D32(S_inst);
    blit_shape_word(s, 0, shape_x(s), shape_y(s));

    s = D32(S_sped);
    s16 len = (s16)(((u32)(u16)(D32(D_speed) >> 16) * (u32)D16(D_speed_bar_scale)) >> 16);
    if (D16(D_speed_bar_dir) == 0) SETD16(D_clip_top, (s16)(DS16(D_speed_bar_x0) - len));
    else                           SETD16(D_clip_right, (s16)(DS16(D_speed_bar_x0) + len));
    blit_shape_xor_word(s, shape_x(s), shape_y(s));
    SETD16(D_clip_top, 0);
    SETD16(D_clip_right, 0x140);

    s = D32(S_tach);
    s16 r = (s16)D16(D_rpm);
    if (r <= 0x320) r = 0x320;
    len = (s16)(((u32)((u16)r >> 4) * (u32)D16(D_tach_bar_scale)) >> 16);
    if (D16(D_tach_bar_dir) == 0) SETD16(D_clip_top, (s16)(DS16(D_tach_bar_x0) - len));
    else                          SETD16(D_clip_right, (s16)(DS16(D_tach_bar_x0) + len));
    blit_shape_xor_word(s, shape_x(s), shape_y(s));
    SETD16(D_clip_top, 0);
    SETD16(D_clip_right, 0x140);

    draw_number((u16)(D32(D_speed) >> 16), S_speed_dgt, D32(D_speed_digit_xy));
    s16 t = (s16)D16(D_rpm);
    if (t <= 0x320) t = 0x320;
    u16 tv = (u16)((u32)(u16)t / 100u);
    if (tv >= 100) tv = 99;
    draw_number(tv, S_tach_dgt, D32(D_tach_digit_xy) - 4);
    if (D32(D_tach_overlay)) blit_at_own(D32(D_tach_overlay), D32(D_tach_overlay_mask));
    if (D32(D_speed_overlay)) blit_at_own(D32(D_speed_overlay), D32(D_speed_overlay_mask));
    blit_wait();
    gfx_DisownBlitter();
}

/* 0x20AF4: pick the wheel-rim overlays for the pose just drawn into this buffer, then the needles (always
 * redrawn: D:1900 is never updated, kept in both modes) or the digital cluster on change. */
void update_gauges(void)
{
    if (DS16(D_wheel_pose_other) != (s16)D16(D_gauge_pose)) {
        SETD16(D_gauge_pose, D16(D_wheel_pose_other));
        s16 i = (s16)(DS16(D_gauge_pose) * 4);
        SETD32(D_speed_overlay, tbl(S_spm, i));
        SETD32(D_speed_overlay_mask, tbl(S_spm_M, i));
        SETD32(D_tach_overlay, tbl(S_tcm, i));
        SETD32(D_tach_overlay_mask, tbl(S_tcm_M, i));
        SETD16(D_needle_speed, 0xFFFF);
        SETD32(D_digital_speed, 0xFFFFFFFFu);           /* both buffers */
    }
    u16 sp = (u16)(D32(D_speed) >> 16);                 /* the live speed, not the frame snapshot */
    if (!D16(D_needle_gauges)) {
        u32 b = 2u * (u32)D16(D_back_index);
        u16 rp = (u16)(D16(D_rpm) & 0xFFE0);
        if (sp == rd16(DADDR(D_digital_speed) + b) && rp == rd16(DADDR(D_digital_rpm) + b)) return;
        wr16(DADDR(D_digital_speed) + b, sp);
        wr16(DADDR(D_digital_rpm) + b, rp);
        draw_digital_cluster();
        return;
    }
    if (sp == D16(D_needle_speed) && (u16)(D16(D_rpm) & 0xFFE0) == D16(D_needle_rpm)) return;
    s16 r = (s16)D16(D_rpm);
    if (r > DS16(D_car_tach_clamp)) r = DS16(D_car_tach_clamp);
    if (r <= 0x320) r = 0x320;
    u16 i = (u16)(((u16)r >> 5) & 0x1FE);
    APTR tips = D32(D_tach_tips);
    draw_needle((s16)(rd8(tips + i) - DS16(D_tach_box_x)),
                (s16)(rd8(tips + i + 1) - DS16(D_tach_box_y)), 0);
    SETD16(D_needle_speed, sp);
    s16 v = (s16)(sp - 10);
    if (v < 0) v = 0;
    tips = D32(D_speed_tips);
    draw_needle((s16)(rd8(tips + 2u * (u32)(u16)v) - DS16(D_speed_box_x)),
                (s16)(rd8(tips + 2u * (u32)(u16)v + 1) - DS16(D_speed_box_y)), 2);
}

/* 0x247B2: the steering angle to the marker dot on sprite 7 (called from the simulation every tick). */
void wheel_marker(void)
{
    s32 a = (s32)((u32)DS32(D_steer) << 1);
    if (a == DS32(D_marker_cache)) return;
    SETD32(D_marker_angle, (u32)a);
    SETD32(D_marker_cache, (u32)a);
    clamp_word(DADDR(D_marker_angle), -0x23, 0x23);
    u16 r = D16(D_marker_r);
    s16 y = (s16)(DS16(D_marker_y) - (s16)(((u32)(u16)cos_deg((s16)DS16(D_marker_angle)) * (u32)r) >> 16));
    s16 x = (s16)(((u32)(u16)sin_deg(abs_l(DS32(D_marker_angle))) * (u32)r) >> 16);
    if (DS16(D_marker_angle) < 0) x = (s16)(-x);
    x = (s16)(x + DS16(D_marker_x));
    position_wheel_marker(x, y);
}

/* ================================================================ one frame (§4.2) */

/* 0x2090C: the pitch-change detector. Its only consumer (0x20940) has no caller, so nothing acts on it. */
static void detect_pitch_bump(s16 unit)
{
    APTR rec = road_record_at(unit);
    SETD16(D_pitch_bump_flag, 0);
    s8 d = (s8)(rd8(DADDR(D_last_pitch)) - rd8(rec + 2));
    if (d < 0) d = (s8)(-d);
    if (d >= 3) SETD16(D_pitch_bump_flag, (u16)-1);
    wr8(DADDR(D_last_pitch), rd8(rec + 2));
}

/* 0x1D9CC: the draw order of the road window. */
static void compose_road_window(void)
{
    s16 unit = (s16)(D32(D_r_road_pos) >> 16);
    detect_pitch_bump(unit);
    set_main_window_clip();
    blit_begin();
    SETD16(D_queue_count, 0);
    clear_window();
    draw_road_main(unit);
    /* 0x1EDAE computes the clear-rect arguments and does nothing with them */
    draw_clouds();
    draw_object_queue();
    draw_bugs();
    draw_road_mirror(unit);
    draw_object_queue();
    draw_cockpit_overlays();
    toggle_gear_box();
    draw_ticket();
}

/* 0x1D392 */
void render_frame(APTR v)
{
    SETD16(D_sprite_shake, 0);
    APTR other = (v == D32(D_g_backView)) ? D32(D_g_frontView) : D32(D_g_backView);
    SETD16(D_horizon_shake, rd16(rd32(other + VIEW_ViewPort) + VP_DyOffset));
    if (DS16(D_horizon_shake) < -10 || DS16(D_horizon_shake) > 10) SETD16(D_horizon_shake, 0);
    blit_set_dest(view_planes(v));
    SETD16(D_back_index, v == DADDR(D_g_viewA) ? 0 : 1);
    /* Forbid / Permit: the port is single-threaded, so the snapshot is a plain copy. */
    SETD32(D_r_speed, D32(D_speed));
    SETD32(D_r_road_pos, D32(D_road_pos));
    SETD32(D_r_lateral, D32(D_car_x));
    SETD32(D_r_steer, D32(D_steer));
    for (int i = 0; i < 6; i++) {
        SETD32(D_r_same_dir + 4 * i, D32(D_sd_pos + 4 * i));
        SETD32(D_r_oncoming + 4 * i, D32(D_on_pos + 4 * i));   /* i = 5 reads past D:0CF2, as the original does */
    }
    gfx_OwnBlitter();
    compose_road_window();
    blit_wait();
    gfx_DisownBlitter();
    update_gauges();
}

/* ---- the Road Drawer (0x1D484, 0x24E94, 0x24F3A), run from the drive flow instead of its own task */

void road_drawer_start(void)
{
    SETD16(D_drawer_paused_ack, 0);
    SETD16(D_drawer_pause, 1);                          /* 0x1C900 clears it when the stage starts */
}

void road_drawer_stop(void)
{
    SETD16(D_drawer_pause, 1);
    SETD16(D_drawer_paused_ack, 1);
}

/* 0x1D484, one pass of the endless loop. */
void road_drawer_step(void)
{
    if (D16(D_drawer_pause)) { SETD16(D_drawer_paused_ack, 1); return; }
    SETD16(D_drawer_paused_ack, 0);
    APTR back = D32(D_g_backView);
    render_frame(back);
    if (D16(D_near_end)) {
        APTR rp = D32(D_g_backRastPort);
        gfx_SetAPen(rp, 8);
        gfx_SetBPen(rp, 0);
        gfx_Move(rp, 0x30, 0x55);
        if (D16(D_g_stage) < 4) gfx_Text(rp, "Pulling into the gas station...", 0x1F);
        else                    gfx_Text(rp, "Pulling into the dealership...", 0x1E);
    }
    show_view(back);
    gfx_WaitTOF();
    SETD32(D_drawer_frames, D32(D_drawer_frames) + 1);
}

/* ================================================================ crash (§4.10) */

/* 0x1FDF4 */
void redraw_cockpit_back(void)
{
    gfx_OwnBlitter();
    blit_wait();
    blit_set_dest(view_planes(D32(D_g_backView)));
    blit_begin();
    draw_cockpit_overlays();
    blit_wait();
    gfx_DisownBlitter();
}

/* 0x1FBD8: seven sets of windscreen cracks in colour 28, the cockpit shapes re-blitted over each one. */
void crash_sequence(void)
{
    APTR rp = exec_AllocMem(RP_SIZE, MEMF_PUBLIC | MEMF_CLEAR);
    if (!rp) ahost_fatal("out of memory (crash)");
    gfx_InitRastPort(rp);
    gfx_SetAPen(rp, 0x1C);
    view_copy(D32(D_g_frontView), D32(D_g_backView));
    SETD32(D_g_frontView, DADDR(D_g_viewA));            /* 0x1FC02; the other branch is unreachable */
    wr8(DADDR(D_gearbox_shown) + 1, rd8(DADDR(D_gearbox_shown)));
    reset_wheel_state(1);
    APTR back = D32(D_g_backView);
    wr32(rp + RP_BitMap, rd32(rd32(rd32(back + VIEW_ViewPort) + VP_RasInfo) + RI_BitMap));
    APTR cr = D32(D_crack_ptr);
    for (s16 k = 0; k < (s16)rd16(DADDR(D_crack_count)); k++) {      /* set 0: a star from point 0 */
        gfx_Move(rp, (s16)((s16)rd16(cr) * 2), (s16)rd16(cr + 2));
        gfx_Draw(rp, (s16)((s16)rd16(cr + 2u * (u32)(u16)(2 * k + 2)) * 2),
                     (s16)rd16(cr + 2u * (u32)(u16)(2 * k + 3)));
    }
    redraw_cockpit_back();
    show_view(back);
    view_copy(D32(D_g_frontView), D32(D_g_backView));
    dos_Delay(2);
    for (s16 s = 1; s < 7; s++) {
        back = D32(D_g_backView);
        wr32(rp + RP_BitMap, rd32(rd32(rd32(back + VIEW_ViewPort) + VP_RasInfo) + RI_BitMap));
        cr = rd32(DADDR(D_crack_ptr) + 4u * (u32)(u16)s);
        for (s16 k = 0; k < (s16)rd16(DADDR(D_crack_count) + 2u * (u32)(u16)s); k++) {
            u32 o = 2u * (u32)(u16)(4 * k);
            gfx_Move(rp, (s16)((s16)rd16(cr + o) * 2), (s16)rd16(cr + o + 2));
            gfx_Draw(rp, (s16)((s16)rd16(cr + o + 4) * 2), (s16)rd16(cr + o + 6));
        }
        redraw_cockpit_back();
        show_view(back);
        view_copy(D32(D_g_frontView), D32(D_g_backView));
        dos_Delay(2);
    }
    exec_FreeMem(rp, RP_SIZE);
}

/* ================================================================ shape loading (§4.11) */

static void find_into(u16 dst, APTR archive, u32 name)
{
    SETD32(dst, find_shape(archive, name));
}

/* mask a run of `n` handles from `src` into `dst` */
static void mask_run(u16 dst, u16 src, int n)
{
    for (int i = 0; i < n; i++)
        SETD32(dst + 4 * i, make_mask(D32(D_mask_pool), D32(src + 4 * i)));
}

/* load `n` shapes named prefix + '0'.. into `dst`, then their masks into `dst + 4n` */
static void load_run(u16 dst, APTR archive, u32 prefix, int n, int first)
{
    for (int i = 0; i < n; i++) find_into((u16)(dst + 4 * i), archive, prefix + (u32)(first + i));
    mask_run((u16)(dst + 4 * n), dst, n);
}

/* 0x24FAA: the cockpit and gauge shapes, all from `<car>Dash.Shp`. */
void load_cockpit_shapes(void)
{
    APTR a = D32(D_dash_shapes);
    find_into(S_hood, a, ID('h', 'o', 'o', 'd'));
    find_into(S_roof, a, ID('r', 'o', 'o', 'f'));
    find_into(S_post, a, ID('p', 'o', 's', 't'));
    find_into(S_mirr, a, ID('m', 'i', 'r', 'r'));
    SETD32(S_hood_M, make_mask(D32(D_mask_pool), D32(S_hood)));
    SETD32(S_roof_M, make_mask(D32(D_mask_pool), D32(S_roof)));
    SETD32(S_post_M, make_mask(D32(D_mask_pool), D32(S_post)));
    SETD32(S_mirr_M, make_mask(D32(D_mask_pool), D32(S_mirr)));
    static const u32 tcm_names[5] = { ID('t','c','m','0'), ID('t','c','m','1'), ID('t','c','m','C'),
                                      ID('t','c','m','2'), ID('t','c','m','3') };
    static const u32 spm_names[5] = { ID('s','p','m','0'), ID('s','p','m','1'), ID('s','p','m','C'),
                                      ID('s','p','m','2'), ID('s','p','m','3') };
    for (int i = 0; i < 5; i++) find_into((u16)(S_tcm + 4 * i), a, tcm_names[i]);
    for (int i = 0; i < 5; i++) find_into((u16)(S_spm + 4 * i), a, spm_names[i]);
    if (!D16(D_needle_gauges)) {                        /* the digital cluster only */
        for (int i = 0; i < 10; i++) {
            u32 p = D32(D_tach_digit_prefix) & 0x7F7F7F00u;
            find_into((u16)(S_tach_dgt + 4 * i), a, p + (u32)('0' + i));
            p = D32(D_speed_digit_prefix) & 0x7F7F7F00u;
            find_into((u16)(S_speed_dgt + 4 * i), a, p + (u32)('0' + i));
        }
        find_into(S_inst, a, ID('i', 'n', 's', 't'));
        find_into(S_sped, a, ID('s', 'p', 'e', 'd'));
        find_into(S_tach, a, ID('t', 'a', 'c', 'h'));
        for (int i = 0; i < 5; i++) {
            if (D32(S_tcm + 4 * i)) SETD32(S_tcm_M + 4 * i, make_mask(D32(D_mask_pool), D32(S_tcm + 4 * i)));
            if (D32(S_spm + 4 * i)) SETD32(S_spm_M + 4 * i, make_mask(D32(D_mask_pool), D32(S_spm + 4 * i)));
        }
        SETD32(D_tach_overlay_mask, D32(S_tcm_M + 4 * 2));
        SETD32(D_speed_overlay_mask, D32(S_spm_M + 4 * 2));
    }
    SETD32(D_tach_overlay, D32(S_tcm + 4 * 2));
    SETD32(D_speed_overlay, D32(S_spm + 4 * 2));
    for (int i = 0; i < 5; i++) {                       /* a missing pose falls back to tcmC / spmC */
        if (!D32(S_spm + 4 * i)) {
            SETD32(S_spm + 4 * i, D32(S_spm + 4 * 2));
            SETD32(S_spm_M + 4 * i, D32(S_spm_M + 4 * 2));
        }
        if (!D32(S_tcm + 4 * i)) {
            SETD32(S_tcm + 4 * i, D32(S_tcm + 4 * 2));
            SETD32(S_tcm_M + 4 * i, D32(S_tcm_M + 4 * 2));
        }
    }
    find_into(S_gnob, a, ID('g', 'n', 'o', 'b'));
    find_into(S_gbox, a, ID('g', 'b', 'o', 'x'));
    for (int i = 0; i < 4; i++) find_into((u16)(S_whl + 4 * i), a, ID('w','h','l','0') + (u32)i);
    for (int i = 0; i < 6; i++) find_into((u16)(S_rad + 4 * i), a, ID('r','a','d','0') + (u32)i);
    mask_run(S_rad_M, S_rad, 6);
}

/* 0x25740: rocks, oil, potholes and gravel (hazard codes 0x20..0x23). */
static void load_scenery_shapes(APTR a)
{
    for (int i = 0; i < 8; i++) find_into((u16)(S_rck + 4 * i), a, ID('r','c','k','A') + (u32)i);
    mask_run(S_rck_M, S_rck, 7);                        /* rckH has no mask */
    load_run(S_oil, a, ID('o','i','l','0'), 5, 0);
    load_run(S_pot, a, ID('p','o','t','0'), 5, 0);
    load_run(S_gra, a, ID('g','r','a','0'), 5, 0);
}

/* 0x259D8: the sign faces, the sign backs of the mirror and the poles. */
static void load_sign_shapes(APTR a)
{
    static const u32 signs[6] = { ID('r','t','n','0'), ID('l','t','n','0'), ID('t','w','t','0'),
                                  ID('s','p','3','0'), ID('s','p','5','0'), ID('s','p','6','0') };
    u16 dst = 0x29DA;
    for (int s = 0; s < 6; s++, dst = (u16)(dst + 0x28)) load_run(dst, a, signs[s], 5, 0);
    for (int s = 0; s < 5; s++, dst = (u16)(dst + 0x28)) load_run(dst, a, ID('g','a','s','0'), 5, 0);
    load_run(dst, a, ID('p','a','s','0'), 5, 0);
    dst = (u16)(dst + 0x28);
    load_run(dst, a, ID('m','r','g','0'), 5, 0);
    for (int i = 0; i < 2; i++) find_into((u16)(S_rsg + 4 * i), a, ID('r','s','g','0') + (u32)i);
    mask_run(S_rsg_M, S_rsg, 2);
    for (int i = 0; i < 5; i++) find_into((u16)(S_pol + 4 * i), a, ID('p','o','l','0') + (u32)i);
    for (int i = 0; i < 2; i++) find_into((u16)(S_pol + 4 * (5 + i)), a, ID('r','p','l','0') + (u32)i);
    mask_run(S_pol_M, S_pol, 7);
}

/* 0x2627C: traffic, police and the light bars. */
static void load_traffic_shapes(APTR a)
{
    static const u16 dst[12] = { 0x2BE2, 0x2C0A, 0x2C32, 0x2C5A, 0x2D72, 0x2D9A,
                                 0x2C82, 0x2CAA, 0x2CD2, 0x2CFA, 0x2D22, 0x2D4A };
    static const u32 name[12] = { ID('r','i','g','0'), ID('t','r','k','0'), ID('s','e','d','0'),
                                  ID('s','d','r','0'), ID('r','x','7','0'), ID('x','7','r','0'),
                                  ID('v','n','f','0'), ID('v','n','r','0'), ID('s','e','d','0'),
                                  ID('s','d','r','0'), ID('s','e','d','0'), ID('s','d','r','0') };
    for (int i = 0; i < 12; i++) load_run(dst[i], a, name[i], 5, 0);
    for (int i = 0; i < 4; i++) {
        find_into((u16)(S_cop + 4 * i), a, ID('c','o','p','0') + (u32)i);
        find_into((u16)(S_cpl + 4 * i), a, ID('c','p','l','0') + (u32)i);
    }
    SETD32(S_cop + 4 * 4, D32(S_cop + 4 * 3));
    SETD32(S_cpl + 4 * 4, D32(S_cpl + 4 * 3));
    mask_run(S_cop_M, S_cop, 5);
    SETD32(S_cop_M + 4 * 4, D32(S_cop_M + 4 * 3));
    for (int i = 0; i < 5; i++) find_into((u16)(S_cpr + 4 * i), a, ID('c','p','r','0') + (u32)i);
    mask_run(S_cpr_M, S_cpr, 5);
    for (int i = 0; i < 5; i++) find_into((u16)(S_clr + 4 * i), a, ID('c','l','r','0') + (u32)i);
    find_into((u16)(S_cpb + 4 * 4), a, ID('c', 'p', 'b', '4'));
    find_into((u16)(S_cpb + 4 * 3), a, ID('c', 'p', 'b', '3'));
}

/* 0x253E6: the cockpit shapes, then everything from `Pics/Road.Shp`. */
void load_road_shapes(void)
{
    load_cockpit_shapes();
    APTR a = D32(D_road_shapes);
    static const u32 clouds[4] = { ID('c','l','d','A'), ID('c','l','d','B'),
                                   ID('c','l','d','0'), ID('c','l','d','1') };
    for (int i = 0; i < 4; i++) find_into((u16)(S_cld + 4 * i), a, clouds[i]);
    mask_run(S_cld_M, S_cld, 4);
    for (int i = 0; i < 4; i++) find_into((u16)(S_pst + 4 * i), a, ID('p','s','t','1') + (u32)i);
    mask_run(S_pst_M, S_pst, 4);
    load_scenery_shapes(a);
    load_sign_shapes(a);
    load_traffic_shapes(a);
    find_into(S_clf0, a, ID('c', 'l', 'f', '0'));
    SETD32(S_clf0_M, make_mask(D32(D_mask_pool), D32(S_clf0)));
    for (int i = 0; i < 4; i++) find_into((u16)(S_wed + 4 * i), a, ID('w','e','d','0') + (u32)i);
    mask_run(S_wed_M, S_wed, 4);
    for (int i = 0; i < 4; i++) find_into((u16)(S_lin + 4 * i), a, ID('l','i','n','A') + (u32)i);
    mask_run(S_lin_M, S_lin, 4);
    find_into(S_bug1, a, ID('b', 'u', 'g', '1'));
    find_into(S_bug1b, a, ID('b', 'u', 'g', '1'));
    SETD32(S_bug1_M, make_mask(D32(D_mask_pool), D32(S_bug1)));
    SETD32(S_bug1b_M, make_mask(D32(D_mask_pool), D32(S_bug1b)));
    for (int i = 0; i < 4; i++) find_into((u16)(S_bugX + 4 * i), a, ID('b','u','g','A') + (u32)i);
    mask_run(S_bugX_M, S_bugX, 4);
    find_into(S_tick, a, ID('t', 'i', 'c', 'k'));
    SETD32(S_tick_M, make_mask(D32(D_mask_pool), D32(S_tick)));
    find_into(D_spr_govr, a, ID('g', 'a', 'm', 'e'));
    SETD32(D_spr_gvrm, make_mask(D32(D_mask_pool), D32(D_spr_govr)));
}
