/* Title sequence and car selection — port of td overlay 1, 0x1AED0-0x1C8F8, and root 0x10B22 res_find_list
 * (port/amiga/spec/title_select.md). show_view 0x1C62E is in platform/video.c.
 *
 * All timing is in vertical blanks (tick_count D:03D8, 60 Hz NTSC). The original's busy waits on tick_count
 * pump the host here (the VBL server only runs from ahost_pump); its WaitTOFs are gfx_WaitTOF. The TDScroller
 * exec task (0x1C83C/0x1C820) runs synchronously: show_car scrolls the old car out completely, then loads and
 * decodes the next one, then scrolls it in (title_select §5), with D:2824 kept as the original sets it. */
#include "game.h"

#include "../agfx.h"
#include "../ahost.h"
#include "../asymbols.h"
#include "../platform/audio.h"
#include "../platform/blit.h"
#include "../platform/platform.h"

#include <stdio.h>

#define ID(a, b, c, d) ((u32)(a) << 24 | (u32)(b) << 16 | (u32)(c) << 8 | (u32)(d))

#define VIEW_A   DADDR(D_g_viewA)
#define VIEW_B   DADDR(D_g_viewB)
#define VP_A     DADDR(D_g_vpA)
#define VP_B     DADDR(D_g_vpB)

/* Shape header: +8 x, +0A y (FORMATS.md). */
static s16 shape_x(APTR s) { return (s16)rd16(s + 8); }
static s16 shape_y(APTR s) { return (s16)rd16(s + 0xA); }

static u32 ticks(void) { return D32(D_tick_count); }

/* `while (t > tick_count) ;` — the original's busy wait for the VBL server. */
static void busy_wait_until(u32 t)
{
    while (t > ticks()) ahost_pump();
}

/* View -> first ViewPort -> RasInfo -> BitMap -> &Planes[0] (the blit_set_dest argument). */
static APTR view_planes(APTR view)
{
    return rd32(rd32(rd32(view + VIEW_ViewPort) + VP_RasInfo) + RI_BitMap) + BM_Planes;
}

static void remake(APTR view, APTR vp)
{
    gfx_MakeVPort(view, vp);
    gfx_MrgCop(view);
}

/* The 5-line hold loop that follows many steps of run_intro: returns 1 when interrupted. */
static int wait_until(u32 t)
{
    do {
        rand16();                                       /* result discarded */
        poll_input();
        if (quit_requested() || joy_fire()) return 1;
        gfx_WaitTOF();
    } while (t > ticks());                              /* unsigned compare */
    return 0;
}

/* ================================================================ res_find_list 0x10B22 (root) */

/* names: 0-terminated list of longs; 0..5 are mode codes (1 store the shape, 2 store a generated mask, 0 end). */
void res_find_list(APTR archive, APTR names, APTR out, APTR pool)
{
    s16 mode = 1;
    do {
        s32 v = (s32)rd32(names);
        names += 4;
        if (v >= 0 && v <= 5) {
            mode = (s16)v;
        } else {
            APTR s = find_shape(archive, (u32)v);
            if (mode == 1) {                            /* may be 0: a missing name stores 0 */
                wr32(out, s);
                out += 4;
            } else if (mode == 2) {
                wr32(out, make_mask(pool, s));
                out += 4;
            }
        }
    } while (mode != 0);
}

/* ================================================================ title sequence */

/* 0x1B2B2: the 'bull' bar slides from x = 0 to its own x in 4-px steps, one step per VBL, drawn into the back
 * view and flipped; the last position is drawn twice so both buffers end with it. Not interruptible. */
void intro_accolade_bull(APTR titleCar)
{
    APTR bull = find_shape(titleCar, ID('b', 'u', 'l', 'l'));
    set_clip_full();
    show_view(VIEW_A);
    s16 donePrev, done = 0, x = 0;
    u32 t0 = ticks(), delay = 1;
    do {
        gfx_OwnBlitter();
        blit_begin();
        blit_set_dest(view_planes(D32(D_g_backView)));
        SETD32(D_g_blitTag, ID('b', 'u', 'l', 'l'));
        blit_shape(bull, 0, x, shape_y(bull));
        blit_wait();
        gfx_DisownBlitter();
        donePrev = done;
        done = x >= shape_x(bull);
        if (!done) x = (s16)(x + 4);
        busy_wait_until(t0 + delay);
        show_view(D32(D_g_backView));
        t0 = ticks();
        gfx_WaitTOF();
        poll_input();
    } while (!donePrev);
}

/* 0x1B396: the window animation, then the car (the whole top ViewPort) drives off to the left. Both views hold
 * Title4 at entry. Not interruptible. */
void intro_testdrive_car(APTR titleCar)
{
    u32 t0 = ticks();
    /* the original pushes no pool argument (garbage, unused: the list has no mode 2) */
    res_find_list(titleCar, DADDR(D_g_titleShapeNames), DADDR(D_g_titleShapes), 0);
    busy_wait_until(t0 + 0x71);                         /* 113 VBL: the starter cranks */
    set_clip_full();
    APTR front = VIEW_A, back = VIEW_B, vpFront = VP_A, vpBack = VP_B, t;
    show_view(front);
    s16 moving = 0, wf = 0, x = 0, done = 0;
    s32 vel = 0;
    u32 delay = 0;                                      /* set before its first use */
    APTR frm = DADDR(D_g_titleShapes), rrm = frm + 5 * 4, wnd = frm + 10 * 4;
    t0 = ticks();
    do {
        gfx_OwnBlitter();
        blit_begin();
        blit_set_dest(rd32(rd32(vpBack + VP_RasInfo) + RI_BitMap) + BM_Planes);
        if (moving == 1) {
            s16 fw = (s16)((s16)((s16)(x + 4) / 5) % 5);   /* divs.w twice: 16-bit signed */
            s16 rw = (s16)(x % 5);
            wr16(vpBack + VP_DxOffset, (u16)-x);
            APTR s = rd32(frm + 4u * (u16)fw);
            SETD32(D_g_blitTag, ID('f', 'w', 'h', 'l'));
            blit_shape_word(s, 0, shape_x(s), shape_y(s));
            s = rd32(rrm + 4u * (u16)rw);
            SETD32(D_g_blitTag, ID('r', 'w', 'h', 'l'));
            blit_shape_word(s, 0, shape_x(s), shape_y(s));
            remake(back, vpBack);
            x = (s16)(x + (s16)(vel >> 16));            /* asr.l #16, add.w */
            vel += 0x3A98;                              /* 15000 */
            done = x >= 0x140;
            delay = 2;
        }
        if (moving == 0 || wf < 0x21) {
            APTR s = rd32(wnd + 4u * (u16)wf);
            SETD32(D_g_blitTag, ID('w', 'n', 'd', 'w'));
            blit_shape_word(s, 0, shape_x(s), shape_y(s));
            wf++;
            if (wf < 0x1A) delay = 9;
            else moving = 1;
        }
        blit_wait();
        gfx_DisownBlitter();
        show_view(back);
        gfx_WaitTOF();
        busy_wait_until(t0 + delay);
        t0 = ticks();
        t = front; front = back; back = t;
        t = vpFront; vpFront = vpBack; vpBack = t;
        poll_input();
    } while (!done);
    SETD16(D_g_vpA_DxOffset, 0);
    SETD16(D_g_vpB_DxOffset, 0);
    view_clear(VIEW_A);
    view_clear(VIEW_B);
    gfx_MakeVPort(VIEW_A, VP_A);
    gfx_MakeVPort(VIEW_B, VP_B);
    gfx_MrgCop(VIEW_A);
    gfx_MrgCop(VIEW_B);
}

/* 0x1B62E: Title3 drops from the top with damped bounces (the physics of DOS 0x0792, one step per VBL, moving
 * the ViewPort's DyOffset), then B gets A scrolled down 160 lines and the KEYS shape is dissolved in. */
void intro_testdrive_logo(APTR titleCar)
{
    s16 phase = 0, settle = 0, accel = 0x30C, y = -0xC3, damp = 7, finished = 0;
    s32 vel = 0;
    u32 t0 = ticks(), delay = 1;
    do {
        SETD16(D_g_vpA_DyOffset, y + 0xA0);             /* set before the step: the display lags one step */
        if (phase == 0) {
            y = (s16)(y + (s16)(vel >> 12));
            vel += accel;
            if (y > 0) {
                accel = (s16)(accel * -7);              /* muls.w: low word */
                phase = 1;
                vel = -vel;
            }
        } else if (phase == 1) {
            s32 prev = y;
            y = (s16)(y + (s16)(vel >> 12));
            vel += accel;
            if ((y >= 0 && prev < 0) || (y < 0 && prev >= 0)) {
                vel = vel / damp;                       /* signed 32-bit divide (0x16ED8) */
                damp++;
                accel = (s16)-accel;
            }
            if (y == 0 && vel < 0xDAC && vel > -0xDAC) phase = 2;
        } else {
            y = 0;
            settle++;
            finished = settle > 5;
        }
        busy_wait_until(t0 + delay);
        remake(VIEW_A, VP_A);
        show_view(VIEW_A);
        t0 = ticks();
        gfx_WaitTOF();
        poll_input();
    } while (!finished && !quit_requested());           /* fire does not interrupt */
    SETD16(D_g_vpA_DyOffset, 0xA0);
    remake(VIEW_A, VP_A);
    show_view(VIEW_A);
    SETD16(D_g_vpB_DyOffset, 0);
    remake(VIEW_B, VP_B);
    view_copy(VIEW_A, VIEW_B);                          /* palettes + bitmaps A -> B */
    SETD32(D_g_tmpRastPort, alloc_public(100));
    APTR rp = D32(D_g_tmpRastPort);
    gfx_InitRastPort(rp);
    wr32(rp + RP_BitMap, DADDR(D_g_bitmapB));
    gfx_SetBPen(rp, 0);
    gfx_ScrollRaster(rp, 0, -0xA0, 0, 0, 0x13F, 0xC7);  /* rows move down 160; rows 0..159 = pen 0 */
    free_mem(rp);
    SETD32(D_g_tmpRastPort, 0);
    show_view(VIEW_B);                                  /* B with DyOffset 0 looks like A with DyOffset 160 */
    view_copy(VIEW_B, VIEW_A);
    poll_input();
    APTR keys = find_shape(titleCar, ID('K', 'E', 'Y', 'S'));
    gfx_OwnBlitter();                                   /* no blit_begin here */
    blit_set_dest(view_planes(D32(D_g_backView)));      /* back = A */
    SETD32(D_g_blitTag, ID('K', 'E', 'Y', 'S'));
    blit_shape_word(keys, 0, shape_x(keys), shape_y(keys));
    blit_wait();
    gfx_DisownBlitter();
    poll_input();
    dos_Delay(0x3C);                                    /* 1.2 s, not interruptible */
    poll_input();
    if (!quit_requested()) {
        view_dissolve(VIEW_A, VIEW_B);                  /* KEYS appears */
        poll_input();
    }
}

/* 0x1AED0: Title2 + bull + Accolade, Title4 + the car driving off, Title3 logo + KEYS.
 * Returns -1 when it ran to the end, 1 when interrupted by fire or Ctrl-C. */
s16 run_intro(void)
{
    poll_input();
    APTR acc = 0, starter = 0, car = 0;
    s16 result = 1;
    u32 t;
    if (joy_fire()) goto out;                           /* fire held: skip everything */
    view_clear(VIEW_A);
    show_view(VIEW_A);
    car = load_file_chip("pics/TitleCar.Shp");
    acc = load_file_chip("sfx/Accolade");
    load_file("pics/Title2");
    ilbm_to_view(D32(D_g_loadedBuf), VIEW_B);
    free_mem(D32(D_g_loadedBuf));
    view_copy_palette(VIEW_B, VIEW_A);
    view_dissolve(VIEW_B, VIEW_A);                      /* Title2 appears on A (shown) */
    poll_input();
    if (D16(D_sfx_enabled)) song_set_volume(7, 0x7000); /* duck the song */
    intro_accolade_bull(car);
    poll_input();
    if (joy_fire()) goto out;
    if (D16(D_sfx_enabled)) play_sample(acc, 3, 0x40);
    t = ticks() + 0xC8;                                 /* 200 VBL from the sample start */
    if (joy_fire()) goto out;
    while (sfx_channel_busy(3)) ahost_pump();           /* busy loop until the Accolade sample ends */
    if (joy_fire()) goto out;
    poll_input();
    song_set_volume(0x20, 0x9000);                      /* not conditional on the sound switch */
    load_file("pics/Title4");
    poll_input();
    ilbm_to_view(D32(D_g_loadedBuf), VIEW_B);
    free_mem(D32(D_g_loadedBuf));
    show_view(VIEW_A);
    starter = load_file_chip("sfx/Starter");
    poll_input();
    if (wait_until(t)) goto out;
    view_dissolve(0, VIEW_A);                           /* Title2 to black */
    poll_input();
    view_copy_palette(VIEW_B, VIEW_A);
    if (D16(D_sfx_enabled)) song_set_volume(7, 0x3500);
    view_dissolve(VIEW_B, VIEW_A);                      /* Title4 appears */
    poll_input();
    if (wait_until(ticks() + 0x50)) goto out;
    if (D16(D_sfx_enabled)) play_sample(starter, 3, 0x40);
    if (joy_fire()) goto out;
    poll_input();
    intro_testdrive_car(car);                           /* not interruptible */
    song_set_volume(0x20, 0x9000);
    if (wait_until(ticks() + 5)) goto out;
    show_view(VIEW_B);
    load_file("pics/Title3");
    poll_input();
    ilbm_to_view(D32(D_g_loadedBuf), VIEW_A);
    free_mem(D32(D_g_loadedBuf));
    poll_input();
    intro_testdrive_logo(car);
    if (wait_until(ticks() + 0x12C)) goto out;          /* 300 VBL = 5 s */
    view_dissolve(0, VIEW_B);                           /* to black */
    poll_input();
    if (!quit_requested()) result = -1;
out:
    if (result != -1) song_set_volume(0, 0x7000);       /* fade the song out when interrupted */
    view_clear(VIEW_A);
    view_clear(VIEW_B);
    SETD16(D_g_vpA_DyOffset, 0);
    remake(VIEW_A, VP_A);
    show_view(VIEW_A);
    if (starter) free_mem(starter);
    if (acc) free_mem(acc);
    if (car) free_mem(car);
    return result;
}

/* ================================================================ car selection */

/* 0x1C15C */
void colormap_clear(APTR cm)
{
    APTR ct = rd32(cm + CM_ColorTable);
    for (u16 i = 0; i < rd16(cm + CM_Count); i++) wr16(ct + 2u * i, 0);
}

/* 0x1BA7C: a 640 x 111 hires 2-plane ViewPort from line 89 under a 320 x 88 top ViewPort, in both views. */
void showroom_display_init(void)
{
    APTR svA = DADDR(D_g_sheetVpA), svB = DADDR(D_g_sheetVpB);
    APTR bmA = DADDR(D_g_sheetBmA), bmB = DADDR(D_g_sheetBmB);
    APTR riA = DADDR(D_g_sheetRasA), riB = DADDR(D_g_sheetRasB);
    view_clear(VIEW_B);
    show_view(VIEW_B);
    view_clear(VIEW_A);
    gfx_InitVPort(svA);
    gfx_InitVPort(svB);
    gfx_InitBitMap(bmA, 2, 0x280, 0x6F);
    gfx_InitBitMap(bmB, 2, 0x280, 0x6F);
    wr32(riA + RI_BitMap, bmA); wr16(riA + RI_RxOffset, 0); wr16(riA + RI_RyOffset, 0); wr32(riA + RI_Next, 0);
    wr32(riB + RI_BitMap, bmB); wr16(riB + RI_RxOffset, 0); wr16(riB + RI_RyOffset, 0); wr32(riB + RI_Next, 0);
    APTR sv[2] = { svA, svB };
    for (int i = 0; i < 2; i++) {
        wr16(sv[i] + VP_DyOffset, 0x59);
        wr16(sv[i] + VP_DWidth, 0x280);
        wr16(sv[i] + VP_DHeight, 0x6F);
        wr16(sv[i] + VP_Modes, V_HIRES);
    }
    wr32(svA + VP_RasInfo, riA);
    wr32(svB + VP_RasInfo, riB);
    gfx_InitRastPort(DADDR(D_g_sheetRpA));
    wr32(DADDR(D_g_sheetRpA) + RP_BitMap, bmA);
    gfx_InitRastPort(DADDR(D_g_sheetRpB));
    wr32(DADDR(D_g_sheetRpB) + RP_BitMap, bmB);
    SETD32(D_g_sheetCmA, gfx_GetColorMap(0x20));
    SETD32(D_g_sheetCmB, gfx_GetColorMap(0x20));
    wr32(svA + VP_ColorMap, D32(D_g_sheetCmA));
    wr32(svB + VP_ColorMap, D32(D_g_sheetCmB));
    colormap_clear(D32(D_g_sheetCmA));
    colormap_clear(D32(D_g_sheetCmB));
    for (int i = 0; i < 2; i++) {
        wr32(bmA + BM_Planes + 4u * i, alloc_chip(0x22B0));
        wr32(bmB + BM_Planes + 4u * i, alloc_chip(0x22B0));
    }
    wr32(VP_A + VP_Next, svA);
    wr32(VP_B + VP_Next, svB);
    SETD16(D_g_vpA_DHeight, 0x58);                      /* top band = 88 lines */
    SETD16(D_g_vpB_DHeight, 0x58);
    gfx_MakeVPort(VIEW_A, VP_A);
    gfx_MakeVPort(VIEW_A, svA);
    gfx_MrgCop(VIEW_A);
    show_view(VIEW_A);
    gfx_MakeVPort(VIEW_B, VP_B);
    gfx_MakeVPort(VIEW_B, svB);
    gfx_MrgCop(VIEW_B);
}

/* 0x1BC64 */
void showroom_display_free(void)
{
    APTR bmA = DADDR(D_g_sheetBmA), bmB = DADDR(D_g_sheetBmB);
    wr32(VP_A + VP_Next, 0);
    wr32(VP_B + VP_Next, 0);
    SETD16(D_g_vpA_DHeight, 200);
    SETD16(D_g_vpB_DHeight, 200);
    gfx_MakeVPort(VIEW_A, VP_A);
    gfx_MakeVPort(VIEW_B, VP_B);
    gfx_MrgCop(VIEW_A);
    gfx_MrgCop(VIEW_B);
    for (int i = 0; i < 2; i++) {
        free_mem(rd32(bmA + BM_Planes + 4u * i));
        free_mem(rd32(bmB + BM_Planes + 4u * i));
    }
    gfx_FreeColorMap(D32(D_g_sheetCmA));
    gfx_FreeColorMap(D32(D_g_sheetCmB));
    gfx_FreeVPortCopLists(DADDR(D_g_sheetVpA));
    gfx_FreeVPortCopLists(DADDR(D_g_sheetVpB));
}

/* 0x1BD14 */
void car_pics_free_all(void)
{
    for (u32 i = 0; i < 0x14; i++) {
        if (D32(D_g_stCache + 4 * i)) {
            free_mem(D32(D_g_stCache + 4 * i));
            SETD32(D_g_stCache + 4 * i, 0);
        }
        if (D32(D_g_sbCache + 4 * i)) {
            free_mem(D32(D_g_sbCache + 4 * i));
            SETD32(D_g_sbCache + 4 * i, 0);
        }
    }
}

/* 0x1BD9C: out of memory -> drop the car picture caches and try once more. */
APTR load_file_retry(const char *name)
{
    APTR p = load_file(name);
    if (!p) {
        car_pics_free_all();
        p = load_file(name);
    }
    return p;
}

/* ---------------------------------------------------------------- .SB spec sheet (hires ILBM) */

/* 0x1C068: ByteRun1 rows; rows below 0x59 are discarded, the rest go into the hires sheet's planes. */
void sb_decode_body(APTR bmhd, const u16 *colors, APTR body, APTR view)
{
    APTR sheet = rd32(rd32(view + VIEW_ViewPort) + VP_Next);
    APTR bm = rd32(rd32(sheet + VP_RasInfo) + RI_BitMap);
    APTR planes[8] = { 0 };
    u8 nplanes = rd8(bmhd + 8);
    for (u16 p = 0; p < nplanes && p < 8; p++) planes[p] = rd32(bm + BM_Planes + 4u * p);
    u16 rowBytes = (u16)((rd16(bmhd) + 7) >> 3);        /* 80 */
    /* the original's scratch row is 100 bytes on the stack; the port's is as long as a row */
    APTR scratch = exec_AllocMem(rowBytes ? rowBytes : 1, MEMF_PUBLIC);
    if (!scratch) return;
    for (u16 row = 0; row < rd16(bmhd + 2); row++)
        for (u16 p = 0; p < nplanes && p < 8; p++) {
            if ((s16)row < 0x59) {
                APTR s = scratch;
                unpack_byterun1_row(&body, &s, (s16)rowBytes);
            } else {
                unpack_byterun1_row(&body, &planes[p], (s16)rowBytes);   /* planes[p] advances */
            }
        }
    exec_FreeMem(scratch, rowBytes ? rowBytes : 1);
    gfx_WaitTOF();
    APTR tmp = exec_AllocMem(0x40, MEMF_PUBLIC);        /* LoadRGB4 reads the colours from 68000 memory */
    if (!tmp) return;
    for (int i = 0; i < 32; i++) wr16(tmp + 2u * i, colors[i]);
    gfx_LoadRGB4(sheet, tmp, 0x20);
    exec_FreeMem(tmp, 0x40);
}

/* 0x1BF8E: the chunk walk of root 0x106BE without CMP2. */
void sb_parse_ilbm(APTR form, APTR view)
{
    APTR bmhd = 0, body = 0;
    u16 colors[32] = { 0 };                             /* entries 3, 7, .. 31 are uninitialised in the original */
    s32 off = 0xC, end = (s32)rd32(form + 4) + 8;
    while (off < end) {
        APTR ch = form + (u32)off;
        u32 id = rd32(ch);
        if (id == ID('B', 'M', 'H', 'D')) bmhd = ch + 8;
        else if (id == ID('B', 'O', 'D', 'Y')) body = ch + 8;
        else if (id == ID('C', 'M', 'A', 'P')) cmap_to_rgb4(ch, colors);
        iff_next_chunk(form, &off);
        if (off >= 0x9C40 || off <= 0) break;
    }
    if (bmhd && body) sb_decode_body(bmhd, colors, body, view);
}

/* 0x1BF40 */
void sb_load_ilbm(APTR data, APTR view)
{
    if (rd32(data) == ID('F', 'O', 'R', 'M') && rd32(data + 8) == ID('I', 'L', 'B', 'M')) sb_parse_ilbm(data, view);
}

/* ---------------------------------------------------------------- scrolling (TDScroller) */

/* 0x1C682: the old picture slides down out of the 88-line band (task body, dir +1). */
void scroll_out_down(APTR v)
{
    APTR vp = rd32(v + VIEW_ViewPort);
    s16 h = (s16)rd16(vp + VP_DHeight);                 /* 0x58 */
    for (s16 i = 0; i < 0x58; i++) {
        wr16(vp + VP_DyOffset, (u16)i);
        wr16(vp + VP_DHeight, (u16)(h - i));
        remake(v, vp);
        show_view(v);
        gfx_WaitTOF();
    }
    wr16(vp + VP_DyOffset, 0);                          /* no MakeVPort afterwards */
    wr16(vp + VP_DHeight, (u16)h);
}

/* 0x1C700: the new picture rises into the band from line 87 (dir -1). */
void scroll_in_up(APTR v)
{
    APTR vp = rd32(v + VIEW_ViewPort);
    s16 h = (s16)rd16(vp + VP_DHeight);
    for (s16 i = 0x57; i >= 0; i--) {
        wr16(vp + VP_DyOffset, (u16)i);
        wr16(vp + VP_DHeight, (u16)(h - i));
        remake(v, vp);
        show_view(v);
        gfx_WaitTOF();
    }
}

/* 0x1C76A: the old picture scrolls up, RyOffset 0..0x58 (task body, dir -1). */
void scroll_out_up(APTR v)
{
    APTR vp = rd32(v + VIEW_ViewPort);
    APTR ri = rd32(vp + VP_RasInfo);
    for (s16 i = 0; i < 0x59; i++) {
        wr16(ri + RI_RyOffset, (u16)i);
        remake(v, vp);
        show_view(v);
        gfx_WaitTOF();
    }
    wr16(ri + RI_RyOffset, 0);
}

/* 0x1C7CC: the new picture scrolls down into place, RyOffset 0x58..0 (dir +1). */
void scroll_in_down(APTR v)
{
    APTR vp = rd32(v + VIEW_ViewPort);
    APTR ri = rd32(vp + VP_RasInfo);
    for (s16 i = 0x58; i >= 0; i--) {
        wr16(ri + RI_RyOffset, (u16)i);
        remake(v, vp);
        show_view(v);
        gfx_WaitTOF();
    }
}

/* 0x1C820: the task body: D:288A(D:2886), then D:2824 = 1. */
static void scroller_task_entry(void)
{
    APTR f = D32(D_g_scrollerFunc), v = D32(D_g_scrollerView);
    if (f == FN_scroll_out_up) scroll_out_up(v);
    else if (f == FN_scroll_out_down) scroll_out_down(v);
    SETD16(D_g_scrollerDone, 1);
}

/* 0x1C83C: builds the "TDScroller" Task (priority 5, 2000-byte stack) as the original does, but instead of
 * AddTask runs its body to the end before returning (title_select §5): the scroll-out is complete before
 * show_car loads the next car. */
void start_scroller(s16 up, APTR v)
{
    SETD32(D_g_savedA4, A4_VALUE);
    SETD32(D_g_scrollerStack, alloc_public(0x7D0));
    if (!D32(D_g_scrollerStack)) return;               /* no scroll-out; D:2824 stays 1 */
    SETD32(D_g_scrollerTask, alloc_public(0x5C));
    if (!D32(D_g_scrollerTask)) return;                /* no scroll-out; show_car frees the stack */
    APTR task = D32(D_g_scrollerTask), stack = D32(D_g_scrollerStack);
    wr8(task + 0x08, 1);                                /* ln_Type = NT_TASK */
    wr32(task + 0x0A, 0x1C8EA);                         /* ln_Name = "TDScroller" (in the overlay code) */
    wr8(task + 0x09, 5);                                /* ln_Pri */
    wr32(task + 0x3A, stack);                           /* tc_SPLower */
    wr32(task + 0x3E, stack + 0x7D0);                   /* tc_SPUpper */
    wr32(task + 0x36, stack + 0x7D0);                   /* tc_SPReg */
    SETD32(D_g_scrollerView, v);
    SETD32(D_g_scrollerFunc, up ? FN_scroll_out_up : FN_scroll_out_down);
    SETD16(D_g_scrollerDone, 0);
    scroller_task_entry();                              /* AddTask(task, 0x1C820, 0) */
}

/* 0x1BDCA: scroll the old car out, load and decode car idx into view (the hidden one), scroll it in. */
void show_car(s16 idx, APTR view, s16 dir)
{
    char name[64], stem[48];
    SETD16(D_g_scrollerDone, 1);
    if (dir == 1) start_scroller(0, D32(D_g_frontView));    /* scroll_out_down */
    if (dir == -1) start_scroller(1, D32(D_g_frontView));   /* scroll_out_up */
    u32 slot = 4u * (u32)(u16)idx;
    amem_cstr(D32(D_g_carNames + slot), stem, sizeof stem);
    if (!D32(D_g_stCache + slot)) {
        snprintf(name, sizeof name, "%s.ST", stem);
        SETD32(D_g_stCache + slot, load_file_retry(name));
    }
    if (!D32(D_g_sbCache + slot)) {
        snprintf(name, sizeof name, "%s.SB", stem);
        SETD32(D_g_sbCache + slot, load_file_retry(name));
    }
    ilbm_to_view(D32(D_g_stCache + slot), view);       /* clears both ViewPorts, .ST into the top one */
    sb_load_ilbm(D32(D_g_sbCache + slot), view);        /* rows 89..199 of .SB into the hires one */
    while (D16(D_g_scrollerDone) == 0) gfx_WaitTOF();
    if (D32(D_g_scrollerStack)) {
        free_mem(D32(D_g_scrollerStack));
        SETD32(D_g_scrollerStack, 0);
    }
    if (D32(D_g_scrollerTask)) {
        free_mem(D32(D_g_scrollerTask));
        SETD32(D_g_scrollerTask, 0);
    }
    if (dir == 0) show_view(view);
    if (dir == -1) scroll_in_up(view);
    if (dir == 1) scroll_in_down(view);
}

/* ---------------------------------------------------------------- showroom drive-away */

/* 0x1C22A */
void wait_frames(s16 n)
{
    while (n-- > 0) gfx_WaitTOF();
}

/* 0x1C190: colours 29-31 of the shown top ViewPort step to black, 12 VBL per step. */
void fade_plate_colours(void)
{
    APTR vp = rd32(D32(D_g_frontView) + VIEW_ViewPort);
    APTR ct = rd32(rd32(vp + VP_ColorMap) + CM_ColorTable);
    u16 any;
    do {
        any = 0;
        for (s16 i = 0x1D; i < 0x20; i++) {
            u16 b = rd16(ct + 2u * (u16)i) & 0xF;
            any |= b;
            if (b) wr16(ct + 2u * (u16)i, (u16)(b - 1));   /* red and green cleared, blue - 1 */
        }
        gfx_LoadRGB4(rd32(D32(D_g_frontView) + VIEW_ViewPort), ct, 0x20);
        wait_frames(0xC);
    } while (any);
}

/* 0x1C244: the <car>.SS script plays the standing animation, then the car drives off to the left. */
void showroom_drive_away(s16 idx)
{
    char name[64], stem[48];
    APTR ent[50] = { 0 };
    s16 nm[2] = { 0, 0 };                               /* N entries, M standing frames */
    fade_plate_colours();
    view_copy(D32(D_g_frontView), D32(D_g_backView));  /* palettes + both bitmaps */
    remake(D32(D_g_backView), rd32(D32(D_g_backView) + VIEW_ViewPort));
    poll_input();
    amem_cstr(D32(D_g_carNames + 4u * (u32)(u16)idx), stem, sizeof stem);
    snprintf(name, sizeof name, "%s.ST.Shp", stem);
    APTR shp = load_file_chip(name);
    poll_input();
    snprintf(name, sizeof name, "%s.SS", stem);
    APTR ss = load_file(name);
    poll_input();
    APTR starter = load_file_chip("sfx/Starter");
    poll_input();
    amem_scan_ints(ss, nm, 2);
    s16 n = nm[0], m = nm[1];
    APTR p = ss;
    skip_line(&p);
    skip_line(&p);
    for (s16 i = 0; i < n; i++) {
        u32 name4 = rd32(p);                            /* 4 name bytes */
        p += 4;
        APTR s = find_shape(shp, name4);
        if (i < 50) ent[i] = s;                         /* the original has no bound: n > 50 overflows ent[] */
        p++;                                            /* one separator byte */
    }
    set_clip_full();
    show_view(D32(D_g_frontView));
    s16 moving = 0, k = 6, x = 0, done = 0;
    s32 vel = 0;
    u32 delay = 0, t0 = ticks();                        /* delay: set before its first use with M >= 8 */
    if (D16(D_sfx_enabled)) play_sample(starter, 3, 0x40);
    do {
        APTR back = D32(D_g_backView);
        gfx_OwnBlitter();
        blit_begin();
        blit_set_dest(view_planes(back));
        if (moving == 1) {
            s16 w = (s16)((s16)((s16)(x + 4) / 5) % 3);   /* 16-bit signed divs */
            wr16(rd32(back + VIEW_ViewPort) + VP_DxOffset, (u16)-x);
            APTR s = ent[w];
            blit_shape_word(s, 0, shape_x(s), shape_y(s));          /* front wheel */
            s = ent[w + 3];
            blit_shape_word(s, 0, shape_x(s), shape_y(s));          /* rear wheel, same phase */
            remake(back, rd32(back + VIEW_ViewPort));
            x = (s16)(x + (s16)(vel >> 16));
            vel += 0x2EE0;                              /* 12000 */
            done = x >= 0x140;
            delay = 2;
        }
        if (moving == 0 || k < n) {
            APTR s = (k >= 0 && k < 50) ? ent[k] : 0;
            blit_shape_word(s, 0, shape_x(s), shape_y(s));
            k++;
            if (k < m) delay = 9;
            else moving = 1;
        }
        blit_wait();
        gfx_DisownBlitter();
        show_view(back);                                /* swaps front and back */
        gfx_WaitTOF();
        busy_wait_until(t0 + delay);
        t0 = ticks();
        poll_input();
        if (D16(D_g_abortKey)) done = 1;
    } while (!done);
    SETD16(D_g_vpA_DxOffset, 0);
    SETD16(D_g_vpB_DxOffset, 0);
    view_clear(VIEW_A);
    view_clear(VIEW_B);
    gfx_MakeVPort(VIEW_A, VP_A);
    gfx_MakeVPort(VIEW_B, VP_B);
    gfx_MrgCop(VIEW_A);
    gfx_MrgCop(VIEW_B);
    free_mem(shp);
    free_mem(ss);
    wait_frames(10);
    sfx_stop_channel(3);                                /* unconditional */
    free_mem(starter);
}

/* 0x1B8E6: the showroom loop. Stick down = next car, up = previous, fire selects (after being seen released).
 * Returns the car index (drive-away already played) or -1 (abort key, Ctrl-C, timeout, end of the attract
 * list). */
s16 car_select(void)
{
    s16 idx = 0, dir = 0;
    u32 deadline;
    (void)rd16(rd32(VIEW_A + VIEW_ViewPort) + VP_DyOffset);   /* read into a local, never used */
    showroom_display_init();
    s16 fireReleased = joy_fire() == 0;
    s16 stickReleased = joy_dir() == 0;
show:
    show_car(idx, D32(D_g_backView), dir);
reset_timeout:
    deadline = ticks() + (u32)(D16(D_demo_mode) ? 0x21C : 0x1770);   /* 540 or 6000 VBL (9 s / 100 s) */
    for (;;) {
        poll_input();
        if (D16(D_g_abortKey)) goto abort;
        if (joy_fire()) {
            SETD16(D_demo_mode, 0);
            if (fireReleased) goto chosen;
        } else {
            fireReleased = 1;
        }
        if (quit_requested()) goto abort;
        if (joy_dir() == 0) stickReleased = 1;
        else SETD16(D_demo_mode, 0);
        if (DS16(D_g_joyDir) == 5 && stickReleased) {  /* down: next car */
            stickReleased = 0;
            idx++;
            dir = 1;
            if (idx >= DS16(D_g_numCars)) idx = 0;
            goto show;
        }
        if (DS16(D_g_joyDir) == 1 && stickReleased) {  /* up: previous car */
            stickReleased = 0;
            idx--;
            dir = -1;
            if (idx < 0) idx = (s16)(DS16(D_g_numCars) - 1);
            goto show;
        }
        rand16();
        dos_Delay(5);                                   /* the loop polls at 10 Hz */
        if (DS16(D_g_joyDir) != 0) goto reset_timeout;  /* a held direction restarts the timeout */
        if (deadline > ticks()) continue;
        /* timeout */
        if (!D16(D_demo_mode)) goto abort;
        if (++idx >= DS16(D_g_numCars)) goto abort;     /* attract: next car until the end of the list */
        dir = 1;
        goto show;
    }
abort:
    idx = -1;
chosen:
    car_pics_free_all();
    poll_input();
    if (idx >= 0 && !D16(D_g_abortKey)) showroom_drive_away(idx);
    view_clear(VIEW_A);
    show_view(VIEW_A);
    showroom_display_free();
    return idx;
}
