/* The screens around the drive — port of td 0x1433A-0x14984 (stage results and the gas station, root hunk) and
 * 0x1CF76 (the dealership ending, overlay 2); see port/amiga/spec/game_flow.md §4.
 *
 * run_game (game/drive.c) calls stage_results after every stage but the last: it shows the `<car>Gas` picture
 * with the stage's sign and scrolls the result messages through a two-line window at the bottom of the screen,
 * and after the last stage it calls dealership_ending, which puts `pics/EndGame` above the last cockpit frame
 * and then the `note` shape.
 *
 * Everything keeps the original's 16-bit arithmetic and its globals at their D: addresses, and every wait counts
 * vertical blanks (tick_count D:03D8, 60 Hz NTSC — port/amiga/README.md, Decisions 1), pumping the host through
 * gfx_WaitTOF / dos_Delay as title.c does. */
#include "game.h"

#include "scene.h"
#include "../agfx.h"
#include "../ahost.h"
#include "../asymbols.h"
#include "../platform/audio.h"
#include "../platform/blit.h"
#include "../platform/platform.h"

#include <stdio.h>
#include <string.h>

/* ---------------------------------------------------------------- globals not in asymbols.h */

#define D_g_resultsImageData    0x0A5C      /* D:0A52 + 0xA: struct Image.ImageData */
#define D_results_TxBaseline    0x31B4      /* D:3176 + 0x3E: the results RastPort's TxBaseline (6) */
#define D_shp_gnob              0x257C      /* drive_scene §4.11: the gear-knob shape */
#define D_end_leftover_2838     0x2838      /* the four write-only leftovers of 0x1CF76 */
#define D_end_leftover_28BE     0x28BE
#define D_end_leftover_28C0     0x28C0
#define D_end_leftover_28C2     0x28C2

/* struct Image (intuition), the one at D:0A52 */
#define IMG_LeftEdge    0x0
#define IMG_TopEdge     0x2
#define IMG_Width       0x4
#define IMG_Height      0x6
#define IMG_Depth       0x8
#define IMG_ImageData   0xA
#define IMG_PlanePick   0xE
#define IMG_PlaneOnOff  0xF

#define CUSTOM_SPR0CTL  0x142

/* ---------------------------------------------------------------- small helpers */

/* View -> first ViewPort -> RasInfo -> BitMap, and the address of its Planes[0] (the blit_set_dest argument). */
static APTR view_bitmap(APTR view)
{
    return rd32(rd32(rd32(view + VIEW_ViewPort) + VP_RasInfo) + RI_BitMap);
}

static APTR view_planes(APTR view) { return view_bitmap(view) + BM_Planes; }

/* 0x176DC exec CopyMemQuick. */
static void copy_mem(APTR src, APTR dst, u32 n)
{
    for (u32 i = 0; i < n; i++) wr8(dst + i, rd8(src + i));
}

/* 0x1436E: quit_requested (Ctrl-C) latched for the whole results screen in D:2822. */
static s16 quit_sticky(void)
{
    if (quit_requested()) SETD16(D_g_quitSeen, 1);
    return (s16)D16(D_g_quitSeen);
}

/* 0x1433A: n vertical blanks, cut short by the fire button or Ctrl-C (quit_requested, not the sticky copy). */
static void wait_vbl_or_fire(s16 n)
{
    for (s16 i = 0; i < n; i++) {
        if (joy_fire()) return;
        if (quit_requested()) return;
        poll_input();
        gfx_WaitTOF();
    }
}

/* ---------------------------------------------------------------- user copper lists
 * Static copies of the drive renderer's copper helpers (0x1EF14, 0x1EFD4, 0x1F05A, 0x1FAD4, 0x1FB4A), which
 * game/scene.c keeps private; the gas station and the ending call them through the call table. */

/* ---------------------------------------------------------------- intuition DrawImage (0x17BDC glue)
 * The only image the game draws is the 320 x 16 one-plane results window (D:0A52, PlanePick 1, PlaneOnOff 0):
 * plane 0 of the destination takes the image's plane, the others are filled with their PlaneOnOff bit, so the
 * window shows the text in colour 1 on colour 0. */
static void draw_image(APTR rp, APTR img, s16 x, s16 y)
{
    APTR bm = rd32(rp + RP_BitMap);
    if (!bm) return;
    s16 x0 = (s16)(x + (s16)rd16(img + IMG_LeftEdge)), y0 = (s16)(y + (s16)rd16(img + IMG_TopEdge));
    s16 w = (s16)rd16(img + IMG_Width), h = (s16)rd16(img + IMG_Height);
    u16 srcbpr = (u16)(((u16)w + 15) / 16 * 2);
    APTR data = rd32(img + IMG_ImageData);
    u8 pick = rd8(img + IMG_PlanePick), onoff = rd8(img + IMG_PlaneOnOff);
    u16 imgdepth = rd16(img + IMG_Depth);
    u16 dstbpr = rd16(bm + BM_BytesPerRow);
    u16 dstrows = rd16(bm + BM_Rows);
    u8 depth = rd8(bm + BM_Depth);
    u16 srcplane = 0;
    for (u8 p = 0; p < depth; p++) {
        APTR src = 0;
        int fill = (onoff >> p) & 1;
        if (((pick >> p) & 1) && srcplane < imgdepth) {  /* past the image's planes: PlaneOnOff again */
            src = data + (u32)srcplane * (u32)srcbpr * (u32)(u16)h;
            srcplane++;
        }
        APTR dplane = rd32(bm + BM_Planes + 4u * (u32)p);
        for (s16 r = 0; r < h; r++) {
            s16 dy = (s16)(y0 + r);
            if (dy < 0 || dy >= (s16)dstrows) continue;
            for (s16 c = 0; c < w; c++) {
                s16 dx = (s16)(x0 + c);
                if (dx < 0 || dx >= (s16)(dstbpr * 8)) continue;
                int bit = fill;
                if (src) bit = (rd8(src + (u32)(u16)r * srcbpr + (u32)((u16)c >> 3)) >> (7 - (c & 7))) & 1;
                APTR a = dplane + (u32)(u16)dy * dstbpr + (u32)((u16)dx >> 3);
                u8 m = (u8)(0x80 >> (dx & 7));
                wr8(a, (u8)(bit ? (rd8(a) | m) : (rd8(a) & ~m)));
            }
        }
    }
}

/* ================================================================ the results window (0x14388, 0x1444C) */

/* 0x14388: one line typed into the hidden rows 16..23 of the 320 x 24 bitmap, then scrolled up 8 pixels, the
 * top 16 rows shown at y 184 after every pixel. 6 VBL per pixel and 40 VBL after the line, or 1 VBL per pixel
 * and no pause once fire has been seen (D:2820 stays 0 from then on). */
static void results_add_line(const char *s)
{
    APTR rp = DADDR(D_g_resultsRastPort);
    if (*s == 0) return;                                /* the empty strings of the line-2 tables add no line */
    gfx_Move(rp, 0, (s16)(DS16(D_results_TxBaseline) + 0x10));
    gfx_Text(rp, s, (u16)strlen(s));
    for (s16 i = 0; i < 8; i++) {
        if (joy_fire() || quit_sticky()) SETD16(D_g_scrollDelay, 0);
        gfx_ScrollRaster(rp, 0, 1, 0, 0, 0x13F, 0x17);
        draw_image(D32(D_g_frontRastPort), DADDR(D_g_resultsImage), 0, 0xB8);
        wait_vbl_or_fire((s16)(DS16(D_g_scrollDelay) * 5 + 1));
    }
    wait_vbl_or_fire((s16)(DS16(D_g_scrollDelay) * 0x28));
}

/* The message tables hold pointers to string literals in the code hunk. */
static void results_add_amem_line(APTR s)
{
    char b[128];
    amem_cstr(s, b, sizeof b);
    results_add_line(b);
}

static APTR msg(u16 table, s16 k) { return rd32(DADDR(table) + 4u * (u32)(u16)k); }

/* 0x1444C: pick the two message lines by average speed (or by the time, when the stage took longer than the
 * limit), then scroll the whole report through the window. */
static void results_text(s32 avgSpeed, s16 secs, s32 score, s16 limit)
{
    char buf[64];                                       /* the 53-space literal at 0x146E6 */
    s16 k = (s16)((u16)rand16() % 3);                   /* one draw for both lines (divu) */
    APTR l1, l2;

    SETD16(D_g_scrollDelay, 1);
    if (secs > limit) {                                 /* the "too slow" tier ends the game after the station */
        l1 = msg(D_MSG_L1_SLOW, k);
        l2 = msg(D_MSG_L2_SLOW, k);
        SETD16(D_g_tooSlow, 1);
    } else if (avgSpeed > 0x69) {                       /* 105 mph */
        l1 = msg(D_MSG_L1_FLYING, k);
        l2 = msg(D_MSG_L2_FLYING, k);
    } else if (avgSpeed > 0x5F) {                       /* 95 */
        l1 = msg(D_MSG_L1_COOKING, k);
        l2 = msg(D_MSG_L2_COOKING, k);
    } else if (avgSpeed > 0x41) {                       /* 65 */
        l1 = msg(D_MSG_L1_LEARNING, k);
        l2 = msg(D_MSG_L2_LEARNING, k);
    } else {
        l1 = msg(D_MSG_L1_GRANDMA, k);
        l2 = msg(D_MSG_L2_GRANDMA, k);
    }

    /* a 320 x 24 one-plane bitmap; the original keeps its BitMap on the stack */
    APTR bm = exec_AllocMem(BM_SIZE, MEMF_PUBLIC | MEMF_CLEAR);
    if (!bm) ahost_fatal("out of memory (results bitmap)");
    APTR rp = DADDR(D_g_resultsRastPort);
    gfx_InitBitMap(bm, 1, 0x140, 0x18);
    gfx_InitRastPort(rp);
    gfx_SetAPen(rp, 1);
    wr32(rp + RP_BitMap, bm);
    APTR plane = alloc_chip(0x3C0);
    wr32(bm + BM_Planes, plane);
    SETD32(D_g_resultsImageData, plane);

    wait_vbl_or_fire(0x1E);                             /* 30 VBL = 0.5 s before the first line */
    results_add_amem_line(l1);
    results_add_amem_line(l2);
    snprintf(buf, sizeof buf, "Your average speed was %ld and it took", (long)avgSpeed);   /* 0x1471C */
    results_add_line(buf);
    if (secs >= 0x78)                                                                      /* 0x14743 */
        snprintf(buf, sizeof buf, "you %2d minutes and %02d seconds.", (int)(secs / 60), (int)(secs % 60));
    else if (secs >= 0x3C)                                                                 /* 0x14765 */
        snprintf(buf, sizeof buf, "you %2d minute and %02d seconds.", (int)(secs / 60), (int)(secs % 60));
    else                                                                                   /* 0x14786 */
        snprintf(buf, sizeof buf, "you %2d seconds.", (int)secs);
    results_add_line(buf);
    snprintf(buf, sizeof buf, "That kind of time is worth");                               /* 0x14797 */
    results_add_line(buf);
    snprintf(buf, sizeof buf, "%ld points.", (long)score);                                 /* 0x147B2 */
    results_add_line(buf);
    if (!D16(D_g_tooSlow))                              /* a second draw for the tip */
        results_add_amem_line(msg(D_MSG_TIPS, (s16)((u16)rand16() % 3)));
    results_add_line("Press the joystick button to continue");                             /* 0x147BE */

    free_mem(plane);
    exec_FreeMem(bm, BM_SIZE);
}

/* ================================================================ the gas station (0x147E4) */

void stage_results(s16 car, s32 avgSpeed, s16 secs, s32 score, s16 limit)
{
    char path[50], name[40];                            /* the original's char path[50] (a local of 0x147E4) */

    song_play_testgas();                                /* 0x12E8A, keeps playing into the next stage's load */
    SETD16(D_g_lives, (u16)(D16(D_g_lives) + 2));
    SETD16(D_g_quitSeen, 0);

    amem_cstr(rd32(DADDR(D_g_carNames) + 4u * (u32)(u16)car), name, sizeof name);
    snprintf(path, sizeof path, "%sgas", name);         /* '%sgas' 0x14970: Cars/<car>Gas */
    load_file(path);
    APTR back = D32(D_g_backView);
    ilbm_to_view(D32(D_g_loadedBuf), back);
    free_mem(D32(D_g_loadedBuf));

    /* the picture's second palette (`CMP2`, parsed by 0x1086E) from line D:281E down */
    APTR ucl = exec_AllocMem(UCL_SIZE, MEMF_CHIP | MEMF_CLEAR);
    if (!ucl) ahost_fatal("out of memory (gas station copper list)");
    ucop_palette_split(ucl, DADDR(D_cmp2_palette), (s16)D16(D_cmp2_line));
    wr32(rd32(back + VIEW_ViewPort) + VP_UCopIns, ucl);
    remake_view(back);                                  /* 0x24D68 */

    load_file_chip("pics/gas.shp");                     /* the four signs: 'don ', 'john', 'kevn', 'tony' */
    APTR sign = find_shape(D32(D_g_loadedBuf), rd32(DADDR(D_GAS_SIGN_NAMES) + 4u * (u32)D16(D_g_stage)));
    draw_shape_on_view(sign, back);                     /* 0x14984, at the shape's own position */
    free_mem(D32(D_g_loadedBuf));
    show_view(back);                                    /* 0x1C62E: the flip onto the gas picture */

    results_text(avgSpeed, secs, score, limit);

    while (!joy_fire() && !quit_sticky()) {             /* 0x148D6: the station waits for the button */
        poll_input();
        dos_Delay(1);
    }

    fade_out_palette(1, DADDR(D_cmp2_palette), (s16)D16(D_cmp2_line));   /* 0x1FEAE, both halves of the split */
    APTR front = D32(D_g_frontView);
    gfx_FreeVPortCopLists(rd32(front + VIEW_ViewPort));
    remake_view(front);
    view_copy(D32(D_g_backView), front);                /* the hidden cockpit view onto the shown one */

    /* the cockpit sprite state follows the buffer swap the flip made */
    if (D32(D_g_backView) == DADDR(D_g_viewA)) SETD8(D_gearbox_shown + 1, D8(D_gearbox_shown));
    else                                                SETD8(D_gearbox_shown, D8(D_gearbox_shown + 1));

    if (D16(D_g_tooSlow))                               /* the run ends: the dashboard palette is not reused */
        for (s16 i = 0; i < 0x20; i++) wr16(DADDR(D_dash_palette) + 2u * (u32)(u16)i, 0);
}

/* ================================================================ the ending (0x1CF76) */

/* 0x1CDFE, inlined at 0x1D10E: the four gear-knob sprites at (D:24B8, D:24B4). */
static void knob_sprites_place(void)
{
    s16 x = DS16(D_knob_x), y = DS16(D_knob_y);
    s16 v1 = (s16)(y + (s16)rd16(D32(D_shp_gnob) + 2));
    sprite_set_segment(rd16(DADDR(D_knob_seg) + 0), x, y, v1, 0, 4);
    sprite_set_segment(rd16(DADDR(D_knob_seg) + 2), x, y, v1, 1, 5);
    sprite_set_segment(rd16(DADDR(D_knob_seg) + 4), (s16)(x + 0x10), y, v1, 0, 6);
    sprite_set_segment(rd16(DADDR(D_knob_seg) + 6), (s16)(x + 0x10), y, v1, 1, 7);
}

void dealership_ending(void)
{
    SETD32(D_mask_pool, arena_new(0x1388));             /* 0x10BA8: the masks of the cockpit shapes */
    load_cockpit_shapes();                              /* 0x24FAA */
    sprites_init();                                     /* 0x1F5BA */

    load_file("pics/EndGame");
    if (D32(D_g_loadedBuf) == 0) goto out;              /* 0x1CFA0 */
    APTR back = D32(D_g_backView), front = D32(D_g_frontView);
    ilbm_to_view(D32(D_g_loadedBuf), back);             /* the dealership, all 200 rows */

    /* keep the cockpit: rows 0x75.. of the last drive frame come back over the picture */
    APTR fbm = view_bitmap(front), bbm = view_bitmap(back);
    for (s16 k = 0; k < 5; k++)
        copy_mem(rd32(fbm + BM_Planes + 4u * (u32)(u16)k) + (u32)(u16)(rd16(fbm + BM_BytesPerRow) * 0x75),
                 rd32(bbm + BM_Planes + 4u * (u32)(u16)k) + (u32)(u16)(rd16(bbm + BM_BytesPerRow) * 0x75),
                 (u32)(u16)((u16)(rd16(bbm + BM_Rows) - 0x75) * rd16(bbm + BM_BytesPerRow)));   /* mulu: 16-bit */
    free_mem(D32(D_g_loadedBuf));

    load_file_chip("pics/EndGame.Shp");
    APTR note = find_shape(D32(D_g_loadedBuf), 0x6E6F7465);        /* 'note' */
    redraw_cockpit_back();                              /* 0x1FDF4 */

    SETD16(D_end_leftover_28C2, 0xFFFF);                /* write-only leftovers (drive_scene §4.12) */
    SETD16(D_end_leftover_28C0, 0xFFFF);
    SETD16(D_end_leftover_28BE, 0xFFFF);
    SETD16(D_needle_rpm, 0xFFFF);                       /* force the needles to be redrawn */
    SETD16(D_end_leftover_2838, 0xFFFF);
    SETD32(D_speed, 0);
    SETD32(D_rpm, 0);                                   /* the long also clears D:191C */
    blit_set_dest(view_planes(back));
    update_gauges();                                    /* 0x20AF4: the gauges at rest */

    APTR ucl = exec_AllocMem(UCL_SIZE, MEMF_CHIP | MEMF_CLEAR);
    if (!ucl) ahost_fatal("out of memory (ending copper list)");
    ucop_sprite_pointers(ucl);                          /* 0x1FAD4 */
    ucop_palette_split(ucl, DADDR(D_dash_palette), 0x75);           /* 0x1FB4A: the dashboard palette */
    wr32(rd32(back + VIEW_ViewPort) + VP_UCopIns, ucl);
    remake_view(back);
    show_view(back);

    if (rd8(DADDR(D_gearbox_show)) != 0) {              /* the gearbox display is on: the knob at its home */
        SETD16(D_knob_x, (u16)(rd16(D32(D_car_knob_xy)) + 0x80 - rd16(D32(D_shp_gnob) + 4)));
        SETD16(D_knob_y, (u16)(rd16(D32(D_car_knob_xy) + 2) + 0x2C - rd16(D32(D_shp_gnob) + 6)));
        knob_sprites_place();
    }

    for (s16 i = 0; i < 0x12C; i++) {                   /* 300 VBL = 5 s, or the button */
        if (quit_requested() || joy_fire()) break;
        gfx_WaitTOF();
        poll_input();
    }

    draw_shape_on_view(note, D32(D_g_frontView));       /* "Nice job. Keep the car. Go home." */
    free_mem(D32(D_g_loadedBuf));
    dos_Delay(10);
    while (!joy_fire() && !quit_requested()) {
        gfx_WaitTOF();
        poll_input();
    }

out:                                                    /* 0x1D24C */
    free_mem(D32(D_mask_pool));
    SETD32(D_mask_pool, 0);
}
