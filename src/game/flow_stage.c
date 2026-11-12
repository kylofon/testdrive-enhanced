/* game_flow: stage loop, scoring, gas station results, per-frame stage runner —
 * port/spec/game_flow.md §4, scene_render.md §4.9, simulation.md §1. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "flow.h"
#include "../host.h"
#include "../keybind.h"
#include "sim_amiga.h"
#include "../enhanced/enhanced.h"
#include "../platform/gfx.h"
#include "../platform/input.h"
#include "../platform/res.h"
#include "../platform/timer.h"

#define DS_STAGE_START 0x6361   /* u16[5] road stream start per stage (read by 0x1F4E and 0x1DC0) */

/* 0x1030 run_game — game_flow.md §4 (verified against disassembly 0x1030-0x125C) */
int run_game(void)
{
    char tmp[30];
    int r;
    s32 score;

    DSS(DS_g_lives) = 5;
    DSL(DS_g_totalScore) = 0;
    DSW(DS_g_tooSlow) = 0;
    DSW(DS_g_stage) = DSW(DS_demo_mode) ? 4 : 0;
    /* ENH developer aid: TD_ENH_STAGE=n (0..4) is the stage the first drive starts on (attract mode: stage 4) */
    static bool stage_done;
    const char *stage = getenv("TD_ENH_STAGE");
    if (stage && !stage_done && atoi(stage) >= 0 && atoi(stage) <= 4) {
        DSW(DS_g_stage) = (u16)atoi(stage);
        stage_done = true;
    }
    for (;;) {
        u16 si = (u16)(DSW(DS_g_stage) * 2);
        DSW(DS_g_parTime) = DSW(DS_STAGE_PAR_TIME + si);
        DSW(DS_g_stageConstB) = DSW(DS_STAGE_CONST_B + si);
        far_wr(DGROUP, DS_g_xroadA, load_archive(DSTR(0x26E), 0x1E84));
        far_wr(DGROUP, DS_g_xroadB, load_archive(DSTR(0x279), 0x17AE));
        far_wr(DGROUP, DS_g_xroadC, load_archive(DSTR(0x284), 0x0FA0));
        snprintf(tmp, sizeof tmp, "%s.pes", flow_car_name(DSS(DS_g_selectedCar)));
        far_wr(DGROUP, DS_g_carArchive, load_archive(tmp, 0x7D0));

        /* car .BIN loader at 0x10E2: open(O_BINARY); read(fd, car_data_ptr(), 0x4D6); close (no checks) */
        snprintf(tmp, sizeof tmp, "%s.bin", flow_car_name(DSS(DS_g_selectedCar)));
        FILE *fb = flow_fopen_game(tmp, false, "rb");
        u16 bin = car_data_ptr();
        if (fb) {
            size_t got = fread(mp(DGROUP, bin), 1, 0x4D6, fb);
            (void)got;
            fclose(fb);
        }

        delay_ticks(400);
        gfx_free_buffer(flow_page_desc());
        r = run_stage();
        far_wr(DGROUP, DS_page_buf_desc, gfx_create_buffer(0x28, 200, 0x0F));
        if (DSW(DS_g_stageTime) == 0) DSW(DS_g_stageTime) = 1;
        s16 t = DSS(DS_g_stageTime);
        DSS(DS_g_avgSpeed) = (s16)(((s32)DSS(DS_g_avgSpeed) * 0x5A) / (s32)t);   /* _aNlmul, _aNldiv */
        DSS(DS_g_stageTime) = (s16)(t / 12);                                    /* cwd; idiv */
        timer_install_menu();                             /* run_stage restored the timer */
        if (r != 1) break;
        score = stage_score();
        if (DSW(DS_demo_mode) != 0) score = 0;
        DSL(DS_g_totalScore) = DSL(DS_g_totalScore) + (u32)score;
        if (DSS(DS_g_stage) >= 4) return 0;
        if (!flow_amiga_stage_results(score)) {
            stage_results(score);
        } else {
            /* The last run_stage() frame — with the "Pulling into the gas station"
             * overlay still drawn in the EGA screen buffer — would otherwise be
             * presented by delay_ticks(400) at the top of the next iteration, which
             * pumps the DOS frame source. Clear the buffer before that happens. */
            gfx_select_target(gfx_screen_desc());
            gfx_clear_screen(0);
            host_present_now();
        }

        if (DSW(DS_g_tooSlow) != 0) return 0;
        DSW(DS_g_stage)++;
    }
    if (r != 0) {                                         /* -1: Esc or demo timeout */
        DSL(DS_g_totalScore) = 0;
        /* Blank the EGA screen buffer and present, so that flow_song_play /
         * flow_amiga_intro pumping before amiga_screens_begin() takes over
         * shows black instead of the stale driving frame. */
        gfx_select_target(gfx_screen_desc());
        gfx_clear_screen(0);
        host_present_now();
        return 1;
    }
    /* game over: wipe to black, unpaced */
    gfx_select_target(flow_page_desc());
    gfx_clear_clip(0);
    gfx_select_target(gfx_screen_desc());
    for (int i = 0; i < 8; i++) {
        set_deadline(1);
        gfx_dissolve(flow_page_sprite(), (u8)i);
        host_present_now();
    }
    return 0;
}

/* 0x125D stage_score — game_flow.md §4 (verified against disassembly 0x125D-0x12DA) */
s32 stage_score(void)
{
    s32 bonus = 0;
    u16 diff = (u16)(DSW(DS_g_stageTime) - DSW(DS_g_parTime));
    s16 d = (s16)(u16)(diff * 12u);                       /* 16-bit multiply (shl/add), then cwd */
    s32 t = d;
    if (t < 100) {
        bonus = 100 - t;
        t = 100;
    }
    s32 factor = DSS(DS_STAGE_SCORE_FACTOR + DSW(DS_g_stage) * 2);
    return (factor * 1000000) / t + bonus;                /* _aNlmul(factor, 0xF4240), _aNldiv */
}

/* 0x1993 stage_results — game_flow.md §4 (verified) */
void stage_results(s32 score)
{   FarPtr scr = gfx_screen_desc();
    char sbName[30];
    DSW(DS_g_scrollDelay) = 15;
    flow_song_play(DS_g_songGas);                         /* AMIGA: or the Amiga song */
    DSS(DS_g_lives) = (s16)(DSS(DS_g_lives) + 2);
    snprintf(sbName, sizeof sbName, "%ssb.pes",
    flow_car_name(DSS(DS_g_selectedCar)));
    memcpy(DSTR(DS_g_sbArchiveName), sbName, strlen(sbName) + 1);
    FarPtr sb = load_archive(DSTR(DS_g_sbArchiveName), 0x109A);          /* "<car>sb.pes" (cached) */
    FarPtr gas = load_archive(DSTR(0x768), 2000);                         /* "gas.pes" */
    FarPtr gcar = res_find(sb, DSTR(0x770));                              /* "gcar" */
    FarPtr bg   = res_find(gas, DSTR(0x775));                             /* "gas " */
    FarPtr sign = res_find(gas, DSTR(DSW(DS_GAS_SIGN_NAMES + DSW(DS_g_stage) * 2)));
    gfx_select_target(flow_page_desc());
    gfx_clear_clip(0);
    gfx_set_clip(scr, 0, 0x28, 0, 200);
    gfx_select_target(scr);
    for (int i = 0; i < 8; i++) {                         /* wipe to black: deadline set, never waited */
        set_deadline(10);
        gfx_dissolve(flow_page_sprite(), (u8)i);
        host_present_now();
    }
    gfx_select_target(flow_page_desc());
    blit_copy_own(bg);
    blit_copy_own(sign);
    blit_copy_own(gcar);
    gfx_select_target(scr);
    for (int i = 0; i < 8; i++) {                         /* wipe in, unpaced */
        set_deadline(10);
        gfx_dissolve(flow_page_sprite(), (u8)i);
        host_present_now();
    }
    results_text(score);
    getkey_wait();
    gfx_set_clip(scr, 0, 0x28, 0, 200);
    gfx_clear_screen(0);
    flow_song_stop();
}

/* 0x1B36 results_text — game_flow.md §4 (thresholds and sprintf arguments verified 0x1B36-0x1CF9) */
void results_text(s32 score)
{
    char buf[80];
    u16 l1, l2;
    gfx_set_text_colours(0x0F, 0);
    s16 secs = DSS(DS_g_stageTime);
    gfx_select_target(flow_page_desc());
    gfx_clear_clip(0);
    DSW(DS_g_resultTextY) = 0;
    DSW(DS_g_resultLineCount) = 0;
    u16 k = (u16)(s16)(crt_rand() % 3);                   /* cwd; idiv 3 */
    s16 avg = DSS(DS_g_avgSpeed);
    if (avg < 50) {
        l1 = DSW(0x792 + k * 2); l2 = DSW(0x7B0 + k * 2);
        DSW(DS_g_tooSlow) = 1;
    } else if (avg > 105) {
        l1 = DSW(0x77A + k * 2); l2 = DSW(0x798 + k * 2);
    } else if (avg > 95) {
        l1 = DSW(0x780 + k * 2); l2 = DSW(0x79E + k * 2);
    } else if (avg > 65) {
        l1 = DSW(0x786 + k * 2); l2 = DSW(0x7A4 + k * 2);
    } else {
        l1 = DSW(0x78C + k * 2); l2 = DSW(0x7AA + k * 2);
    }
    delay_ticks(50);
    results_add_line(DSTR(l1));
    results_add_line(DSTR(l2));
    snprintf(buf, sizeof buf, "Your average speed was %d and it took", (int)avg);           /* DS:07C4 */
    results_add_line(buf);
    if (secs >= 120)
        snprintf(buf, sizeof buf, "you %2d minutes and %02d seconds.", secs / 60, secs % 60); /* DS:07EA */
    else if (secs >= 60)
        snprintf(buf, sizeof buf, "you %2d minute and %02d seconds.", secs / 60, secs % 60);  /* DS:080C */
    else
        snprintf(buf, sizeof buf, "you %2d seconds.", (int)secs);                             /* DS:082D */
    results_add_line(buf);
    snprintf(buf, sizeof buf, "That kind of time is worth");                                  /* DS:083E */
    results_add_line(buf);
    snprintf(buf, sizeof buf, "%ld points.", (long)score);                                    /* DS:0859 */
    results_add_line(buf);
    if (DSW(DS_g_tooSlow) == 0) {
        u16 tip = (u16)(s16)(crt_rand() % 3);
        results_add_line(DSTR(DSW(0x7B6 + tip * 2)));
    }
    results_add_line(DSTR(0x865));                        /* "Press key or joystick button to continue" */
    results_scroll();
}

/* 0x1CFA results_add_line — game_flow.md §4 (verified) */
void results_add_line(const char *s)
{
    if (*s != 0) {
        gfx_draw_text(s, 0, DSS(DS_g_resultTextY));
        DSW(DS_g_resultTextY) = (u16)(DSW(DS_g_resultTextY) + 8);
        DSW(DS_g_resultLineCount)++;
    }
}

/* 0x1D20 results_scroll — game_flow.md §4 (verified) */
void results_scroll(void)
{
    s16 y = 199;
    gfx_select_target(gfx_screen_desc());
    gfx_set_clip(gfx_screen_desc(), 0, 0x28, 0xB0, 200);
    for (s16 line = 0; line < DSS(DS_g_resultLineCount); line++) {
        for (s16 j = 0; j < 8; j++) {
            set_deadline(DSW(DS_g_scrollDelay));
            blit_copy_clip_hot(flow_page_sprite(), 0, y);    /* page top at y */
            if (menu_key() != -1) DSW(DS_g_scrollDelay) = 1; /* any key speeds up, sticky */
            y--;
        }
        delay_ticks(100);                                    /* 1 s per line, not skippable */
    }
}

/* 0x1DC0 run_stage — game_flow.md §4, scene_render.md §4.9 (verified against disassembly 0x1DC0-0x1F4D) */
int run_stage(void)
{
    int r;
    stage_init_road_ptr();
    /* ENH developer aid: TD_ENH_UNIT=n starts the first stage n road units in (the wide roads of the Amiga
     * features begin at unit 1484 of stage 1). */
    static bool unit_done;
    const char *unit = getenv("TD_ENH_UNIT");
    if (unit && !unit_done) {
        DSW(DS_road_pos) = (u16)(DSW(DS_road_pos) + atoi(unit));
        unit_done = true;
    }
    reset_car_state();
    stage_enter_install_isr();
    select_screen();
    gfx_set_clip(gfx_screen_desc(), 0, 0x28, 0x13, 200);
    gfx_clear_clip(8);
    blit_copy_own(far_rd(DGROUP, DS_car_handles));       /* dash */
    blit_copy_own(far_rd(DGROUP, DS_spr_roof));
    DSW(DS_g_stageTime) = 0;
    kb_set_active(true);                                  /* PORT: key bindings while driving */
    sim_amiga_stage_begin();                              /* AMIGA */
    enh_stage_begin();                                    /* ENH: enhanced renderer */
    for (;;) {
        host_frame_begin();                               /* PORT: ticks (and the sim ISR) advance here; paced
                                                             to the emulated original frame rate */
        snapshot_sim_state();
        project_road_main();
        project_road_mirror();
        select_road_buffer();
        gfx_clear_clip(8);
        fill_scenery_above_road();
        draw_road_main();
        draw_mirror_background();
        draw_road_mirror();
        draw_buffer_overlays();
        present_road_buffer();
        enh_frame();
        draw_dashboard_dynamic();
        enh_gear_box();
        rand8();
        if (DSW(DS_demo_mode) != 0 && DSS(DS_g_stageTime) > 0x2D0) { r = -1; break; }   /* signed jg */
        s8 ev = DSC(DS_g_stageEvent);
        if (ev == 0) continue;
        if (ev == 1) { r = -1; break; }                   /* Esc / any key in demo */
        if (ev <= 2) {                                    /* signed byte jg: >= 0x80 also lands here */
            if (DSW(DS_g_carSpeedFixed) != 0) continue;   /* wait until the car has stopped */
            snd_stop_all();
            DSW(DS_g_avgSpeed) = (u16)(DSW(DS_road_pos) - DSW(DS_STAGE_START + DSW(DS_g_stage) * 2) - 0x2D);
            if (DSW(DS_g_stage) == 4) {
                enh_stage_end();
                flow_ending_song(true);                   /* AMIGA: EndSuccess with the Amiga music */
                dealership_ending();
                flow_ending_song(false);
            }
            r = 1;
            break;
        }
        /* ev >= 3: crash */
        snd_stop_all();
        DSB(DS_clock_running) = 0;
        DSW(DS_g_stageTime) = (u16)(DSW(DS_g_stageTime) + 0xF0);
        if (DSW(DS_g_stageTime) > STAGE_CLOCK_MAX) DSW(DS_g_stageTime) = STAGE_CLOCK_MAX;   /* ENH: 45:30 */
        snapshot_sim_state();
        enh_crash_sequence();
        DSW(DS_g_lives) = (u16)(DSW(DS_g_lives) - 1);
        if (DSW(DS_g_lives) == 0) {
            enh_cover_sprite(far_rd(DGROUP, DS_spr_gvrm));
            blit_and_own(far_rd(DGROUP, DS_spr_gvrm));    /* GAME OVER mask */
            blit_or_own(far_rd(DGROUP, DS_spr_govr));
            wait_fire_button();
            r = 0;
            break;
        }
        reset_car_state();
        enh_life_reset();
        snd_set_loop(far_make(DGROUP, DS_SONG_INGAME));
    }
    kb_set_active(false);
    snd_stop_all();
    enh_stage_end();
    /* the three descriptors are offsets into the code-segment pool (push cs; push [DS:08B2]) — LIFO */
    gfx_free_buffer(far_make(CODE_SEG, DSW(DS_buf_desc_c)));
    gfx_free_buffer(far_make(CODE_SEG, DSW(DS_buf_desc_b)));
    gfx_free_buffer(far_make(CODE_SEG, DSW(DS_road_buf_desc)));
    /* PORT: int 21h/25h restores of int 0 (DS:0934) and int 8 (CS:3B18) replaced by removing the ISR. */
    timer_set_driving_isr(NULL);
    timer_restore();                                      /* 0x694A: back to the BIOS timer */
    return r;
}
