/* game_flow: car select, showroom, credits — port/spec/game_flow.md §4. */
#include <stdio.h>
#include <string.h>

#include "flow.h"
#include "../platform/gfx.h"
#include "../platform/input.h"
#include "../platform/res.h"
#include "../platform/timer.h"

/* 0x098C car_select — game_flow.md §4 (verified) */
int car_select(void)
{
    s16 idx = 0;
    int k = -1;
    gfx_clear_screen(0);
    flow_song_play(DS_g_songCarSel);                     /* AMIGA: or the Amiga song */
    show_car(0, 0);
    for (;;) {
        set_deadline(12000);
        kbd_flush();
        if (DSW(DS_demo_mode) == 0) {
            k = menu_key();
        } else {
            s16 n = (s16)((crt_rand() & 3) + 5);
            for (;;) {
                set_deadline(400);
                if (n == 0) break;
                k = menu_key();
                if (k != -1) { DSW(DS_demo_mode) = 0; break; }
                idx++;
                if (DSS(DS_g_numCars) <= idx) idx = 0;
                show_car(idx, -1);
                n--;
            }
        }
        if (k == -1) {
            showroom_drive_away(idx);
            DSS(DS_g_selectedCar) = idx;
            DSW(DS_g_carAutoSelected) = 1;
            flow_song_stop();
            return -1;
        }
        if (k == 1) { flow_song_stop(); return 1; }
        if (k == 0x4800) {
            idx++;
            if (DSS(DS_g_numCars) <= idx) idx = 0;
            show_car(idx, -1);
            continue;
        }
        if (k == 0x5000) {
            idx--;
            if (idx < 0) idx = (s16)(DSS(DS_g_numCars) - 1);
            show_car(idx, 1);
            continue;
        }
        if (k == 0x0D) {
            showroom_drive_away(idx);
            DSS(DS_g_selectedCar) = idx;
            DSW(DS_g_carAutoSelected) = 0;
            flow_song_stop();
            return 0;
        }
    }
}

/* 0x0AB2 show_car — game_flow.md §4 (scroll arguments verified against disassembly 0x0AB2-0x0B99) */
void show_car(int idx, int dir)
{
    FarPtr scr = gfx_screen_desc();
    FarPtr none = far_make(0, 0);
    FarPtr old = flow_page_sprite();                     /* page contents = previous car image */
    gfx_set_clip(scr, 0, 0x28, 0, 0x58);                 /* top 88 lines */
    if (dir == 1) {
        for (s16 i = -0x57; i < 1; i++) {
            set_deadline(1);
            gfx_scroll_window(0, 0x57, 0x28, 0x57, -0x28, none, 0);
            wait_deadline();
            gfx_set_clip(scr, 0, 0x28, 0, 0x58);
        }
    } else if (dir == -1) {
        for (s16 i = 0x57; i >= 0; i--) {
            set_deadline(1);
            gfx_scroll_window(0, 0, 0x28, 0x57, 0x28, none, 0);
            wait_deadline();
            gfx_set_clip(scr, 0, 0x28, 0, 0x58);
        }
    }
    {
        char tmp[30];                                    /* sprintf(g_sbArchiveName, "%ssb.pes", name) DS:022C */
        int n = snprintf(tmp, sizeof tmp, "%ssb.pes", flow_car_name(idx));
        if (n < 0) n = 0;
        if (n > (int)sizeof tmp - 1) n = (int)sizeof tmp - 1;
        tmp[n] = 0;
        memcpy(DSTR(DS_g_sbArchiveName), tmp, (size_t)n + 1);
    }
    far_wr(DGROUP, DS_g_carSBArchive, load_archive(DSTR(DS_g_sbArchiveName), 0x109A));
    FarPtr sb = far_rd(DGROUP, DS_g_carSBArchive);
    far_wr(DGROUP, DS_g_showroomCarSpr, res_find(sb, DSTR(0x235)));      /* "car " */
    FarPtr stat = res_find(sb, DSTR(0x23A));                             /* "stat" */
    FarPtr name = res_find(sb, DSTR(0x23F));                             /* "name" */
    gfx_select_target(flow_page_desc());
    gfx_clear_clip(0);
    blit_copy_own(far_rd(DGROUP, DS_g_showroomCarSpr));
    blit_copy_own(name);
    gfx_set_clip(scr, 0, 0x28, 0, 0x58);
    gfx_select_target(scr);
    if (dir == 0) {
        blit_copy_own(old);                              /* page at (0,0), clipped to 88 lines */
    } else if (dir == 1) {
        for (s16 i = -0x57; i < 1; i++) {
            set_deadline(1);
            gfx_scroll_window(0, 0x57, 0x28, 0x57, -0x28, old, (s16)-i);
            wait_deadline();
            gfx_set_clip(scr, 0, 0x28, 0, 0x58);
        }
    } else if (dir == -1) {
        for (s16 i = 0x57; i >= 0; i--) {
            set_deadline(1);
            gfx_scroll_window(0, 0, 0x28, 0x57, 0x28, old, (s16)(0x57 - i));
            wait_deadline();
            gfx_set_clip(scr, 0, 0x28, 0, 0x58);
        }
    }
    gfx_set_clip(scr, 0, 0x28, 0, 200);
    gfx_select_target(scr);
    blit_copy_own(stat);                                 /* spec sheet */
}

/* 0x0D75 showroom_drive_away — game_flow.md §4 (verified) */
void showroom_drive_away(int idx)
{
    char fn[30], frm_names[40], rrm_names[40], wnd_names[140];
    FarPtr frm[3], rrm[3], wnd[20];                      /* original: 6, 6 and 41 words */
    int wnd_count = 0, start_frame = 0;
    FlowText f;

    memset(frm_names, 0, sizeof frm_names);
    memset(rrm_names, 0, sizeof rrm_names);
    memset(wnd_names, 0, sizeof wnd_names);
    snprintf(fn, sizeof fn, "%s.ss", flow_car_name(idx));                /* DS:0244 */
    if (!flow_text_open(&f, fn)) fatal("%s", DSTR(0x24C));               /* "Animation file open error" */
    flow_text_scan_2d(&f, &wnd_count, &start_frame);                     /* "%d %d\n" */
    read_line_strip(frm_names, 30, &f);
    read_line_strip(rrm_names, 30, &f);
    read_line_strip(wnd_names, 0x82, &f);
    flow_text_close(&f);

    FarPtr sb = far_rd(DGROUP, DS_g_carSBArchive);
    int nfrm = flow_find_list(sb, frm_names, frm, 3);
    int nrrm = flow_find_list(sb, rrm_names, rrm, 3);
    int nwnd = flow_find_list(sb, wnd_names, wnd, 20);

    gfx_select_target(flow_page_desc());
    gfx_clear_clip(0);
    blit_copy_own(far_rd(DGROUP, DS_g_showroomCarSpr));
    FarPtr pg = flow_page_sprite();
    s16 done = 0, moving = 0, dist = 0, vel = 0, wf = 0;
    FarPtr scr = gfx_screen_desc();
    while (!done) {
        gfx_select_target(flow_page_desc());
        if (moving == 1) {
            s16 fi = (s16)(((dist + 4) / 5) % 3);        /* quirk: % 3 here (the intro uses % 5) */
            if (dist % 3 < nrrm) blit_copy_own(rrm[dist % 3]);   /* PORT: bounds guard */
            if (fi < nfrm) blit_copy_own(frm[fi]);
            dist = (s16)(dist + (vel >> 4));
            vel = (s16)(vel + 10);
            done = dist > 0x13F;
            set_deadline(6);
        }
        if (moving == 0 || wf < wnd_count) {
            if (wf < nwnd) blit_copy_clip_own(wnd[wf]); /* PORT: bounds guard */
            set_deadline(0x1E);                          /* overrides the 6 above */
            wf++;
            if (start_frame <= wf) moving = 1;
        }
        gfx_set_clip(scr, 0, 0x28, 0, 0x58);
        gfx_select_target(scr);
        blit_copy_clip_raw(pg, (s16)(0 - dist), 0);
        wait_deadline();                                 /* not interruptible */
    }
    gfx_set_clip(scr, 0, 0x28, 0, 200);
    delay_ticks(100);
}

/* 0x1000 read_line_strip — game_flow.md §4 (verified) */
void read_line_strip(char *buf, int n, FlowText *f)
{
    flow_text_gets(buf, n, f);
    size_t len = strlen(buf);
    if (len) buf[len - 1] = 0;                           /* PORT: the original writes buf[-1] when empty */
}

/* 0x186E credits_show — game_flow.md §4 (verified) */
int credits_show(void)
{
    gfx_clear_screen(0);
    gfx_set_text_colours(0x0F, 1);
    draw_text_centered(DSTR(0x3EA), 0x00);               /* " Created by: " */
    draw_text_centered(DSTR(0x3F8), 0x24);               /* " Design and Programming " */
    draw_text_centered(DSTR(0x411), 0x70);               /* " Art: " */
    draw_text_centered(DSTR(0x418), 0x94);               /* " Sound and Music: " */
    gfx_set_text_colours(0x0F, 0);
    draw_text_centered(DSTR(0x42B), 0x0C);
    draw_text_centered(DSTR(0x445), 0x14);
    draw_text_centered(DSTR(0x454), 0x30);
    draw_text_centered(DSTR(0x461), 0x38);
    draw_text_centered(DSTR(0x46C), 0x40);
    draw_text_centered(DSTR(0x47A), 0x48);
    draw_text_centered(DSTR(0x484), 0x50);
    draw_text_centered(DSTR(0x491), 0x58);
    draw_text_centered(DSTR(0x49C), 0x60);
    draw_text_centered(DSTR(0x4A9), 0x7C);
    draw_text_centered(DSTR(0x4B7), 0x84);
    draw_text_centered(DSTR(0x4C0), 0xA0);
    draw_text_centered(DSTR(0x4CE), 0xA8);
    set_deadline(1000);
    return menu_key();
}
