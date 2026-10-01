/* The drive's flow — port of td 0x1C900 run_game, 0x24ACA stage_load, 0x24CE4 stage_unload, 0x1D560
 * stage_score and 0x1D5C8 play_again_menu (port/amiga/spec/game_flow.md §4). The simulation is game/sim.c,
 * the picture game/scene.c, the drive's sounds platform/audio_drive.c.
 *
 * Frame structure. The original runs the logic in the main task (priority 4) once per *drive frame* — after a
 * frame's work it calls WaitTOF until 5 vertical blanks have passed since the previous frame started, with no
 * catch-up — while a second exec task, "Road Drawer" (priority -1, body 0x1D484), renders into the back View
 * and flips as fast as the CPU allows, one WaitTOF per rendered frame. The port is single-threaded: the wait
 * loop of the drive frame runs drive_tick() once per 60 Hz host tick, and drive_tick does exactly what the
 * drawer's loop body does for one frame (the D:24AE / D:2844 pause handshake, road_drawer_step, D:28D6) and
 * then waits for the vertical blank. So the logic runs at 12 Hz (NTSC, port/amiga/README.md Decisions 1) and
 * the picture at 60 Hz, and the stage clock counts drive frames with 12 frames to a displayed second.
 *
 * README bug 4 (the play-again menu's dead timeout) is fixed here by default; g_original_bugs restores it. */
#include "game.h"

#include <stdio.h>
#include <string.h>

#include "scene.h"
#include "sim.h"
#include "../agfx.h"
#include "../ahost.h"
#include "../aport.h"
#include "../asymbols.h"
#include "../platform/audio.h"
#include "../platform/audio_drive.h"
#include "../platform/blit.h"
#include "../platform/fixed.h"
#include "../platform/platform.h"

/* Globals of the drive that asymbols.h does not name (write-only or drive_scene internals). */
#define D_pitch_rate       0x283A    /* cleared at each life start (0x1C9B0) */
#define D_pitch_prev       0x283E    /* cleared at each life start (0x1C9A8) */
#define D_wheel_angle_acc  0x28B2    /* cleared by stage_load (0x24AE6) */

#define RP_TxBaseline      0x3E      /* struct RastPort, graphics.library 1.3 */

/* Parts of the renderer that game/scene.h does not expose; the names are drive_scene's (port/amiga/symbols.csv,
 * FN_sprites_init / FN_sprites_free / FN_setup_drive_copper / FN_fade_out_drive / FN_load_road_shapes). */
void sprites_init(void);                 /* 0x1F5BA */
void sprites_free(void);                 /* 0x1F99A */
void setup_drive_copper(APTR view);      /* 0x24D8A: dash palette capture, user copper list, road palette */
void fade_out_drive(void);               /* 0x24E52 */
void load_road_shapes(void);             /* 0x253E6: Road.Shp + Dash.Shp handles and masks */

static void stage_load(s16 car);         /* 0x24ACA */
static void stage_unload(s16 keep);      /* 0x24CE4 */
static s32  stage_score(s32 frames, s32 par, s16 factor);   /* 0x1D560 */

/* ================================================================ helpers */

/* View -> ViewPort -> RasInfo -> BitMap -> &Planes[0] (the blit_set_dest argument, as 0x1CBA0 walks it). */
static APTR view_planes(APTR view)
{
    return rd32(rd32(rd32(view + VIEW_ViewPort) + VP_RasInfo) + RI_BitMap) + BM_Planes;
}

/* `sprintf(path, fmt, g_carNames[car])`, the three forms stage_load uses: "%s.b", "%sDash.Shp", "%sDash". */
static void car_path(char *out, size_t n, s16 car, const char *suffix)
{
    char stem[80];
    amem_cstr(rd32(DADDR(D_g_carNames) + 4u * (u32)(u16)car), stem, sizeof stem);
    snprintf(out, n, "%s%s", stem, suffix);
}

/* One 60 Hz tick of the port's single thread: the Road Drawer's loop body 0x1D484 for one frame, then the
 * main task's WaitTOF 0x17B9A. road_drawer_step() is render_frame 0x1D392 plus the "Pulling into the gas
 * station / dealership" line (drawn when D:0B2E is set, with D:24CC choosing the wording) and show_view. */
static void drive_tick(void)
{
    if (D16(D_drawer_pause)) {                       /* D:24AE, also set by the P key in poll_input */
        SETD16(D_drawer_paused_ack, 1);              /* 0x1D48E: the drawer's spin loop */
        ahost_wait_tof();                            /* the spin loop's WaitTOF */
    } else {
        SETD16(D_drawer_paused_ack, 0);              /* 0x1D496 */
        road_drawer_step();                          /* draws, shows, waits for the blank and counts D:28D6 */
    }
}

/* 0x24F3A stop_road_drawer. The original spins on the drawer's acknowledge D:2844; with one thread the drawer
 * is idle by construction, so the acknowledge is set before the call. */
static void drawer_stop(void)
{
    SETD16(D_drawer_paused_ack, 1);
    road_drawer_stop();
}

/* The original's `while (!joy_fire() && !quit_requested()) poll_input();` at CPU speed (0x1CBEA). The Road
 * Drawer is stopped here, so nothing is drawn; the port pumps the host so the window stays alive. */
static void wait_for_fire(void)
{
    while (!joy_fire() && !quit_requested()) {
        poll_input();
        ahost_pump();
    }
}

/* ================================================================ stage_score 0x1D560 */

/* factor * 1000000 / max(frames - par, 100), plus 100 points per frame under par + 100. All signed 32-bit;
 * the divide is the Aztec runtime's 0x16ED8 (truncating toward zero) and the multiply 0x169F0. */
static s32 stage_score(s32 frames, s32 par, s16 factor)
{
    s32 bonus = 0, d = frames - par;
    if (d < 0x64) {
        bonus = 0x64 - d;
        d = 0x64;
    }
    s32 s = ldiv_68k((s32)((u32)(s32)factor * 0xF4240u), d);
    if (bonus != 0) s += (s32)((u32)bonus * 0x64u);
    return s;
}

/* ================================================================ stage_load 0x24ACA / stage_unload 0x24CE4 */

/* Loads everything the drive needs, once per game (D:27CE non-zero afterwards), and starts the Road Drawer
 * (paused). Later lives and stages only rebuild the sprites, clouds and the cockpit view. No load delay (the
 * DOS game waits 4 s) and the car record is not re-read between stages. */
static void stage_load(s16 car)
{
    char path[100];
    SETD32(D_heading, 0);                            /* D:28C6 */
    SETD32(D_pitch_acc, 0);                          /* D:28CA */
    SETD32(D_car_x, 0x640000);                       /* D:2896: 100.0, the middle of the lane */
    SETD32(D_steer, 0);                              /* D:28AA */
    SETD32(D_speed, 0);                              /* D:28A6 */
    SETD32(D_wheel_angle_acc, 0);                    /* D:28B2 */

    if (D32(D_dash_shapes) == 0) {                   /* D:27CE: the first life of the game */
        view_clear(DADDR(D_g_viewA));
        view_clear(DADDR(D_g_viewB));
        gfx_Move(DADDR(D_g_rpA), 100, 100);
        gfx_SetAPen(DADDR(D_g_rpA), 1);
        gfx_Text(DADDR(D_g_rpA), "Loading Game...", 15);
        gfx_LoadRGB4(rd32(DADDR(D_g_viewA) + VIEW_ViewPort), DADDR(D_loading_palette), 2);   /* 000, D04 */
        show_view(DADDR(D_g_viewA));

        SETD32(D_road_shapes, load_file_chip("Pics/Road.Shp"));      /* D:27D2 */
        car_path(path, sizeof path, car, ".b");
        SETD32(D_g_carRecord, load_file(path));                      /* D:2548 */
        car_record_copy();                                           /* 0x20D68 */
        SETD8(D_gearbox_shown + 1, 0);                               /* D:0B33 */
        SETD8(D_gearbox_shown, 0);                                   /* D:0B32 */
        car_path(path, sizeof path, car, "Dash.Shp");
        SETD32(D_dash_shapes, load_file_chip(path));
        SETD32(D_mask_pool, arena_new(0x7918));                      /* D:276E */
        load_road_shapes();                                          /* 0x253E6 */
        car_path(path, sizeof path, car, "Dash");
        load_file(path);                                             /* the ILBM lands in D:24BC */

        sprites_init();                                              /* 0x1F5BA */
        init_clouds();                                               /* 0x1EB78 */
        reset_wheel_state(0);                                        /* 0x1ED8A */
        ilbm_to_view(D32(D_g_loadedBuf), D32(D_g_backView));
        setup_drive_copper(D32(D_g_backView));                       /* 0x24D8A */
        render_frame(D32(D_g_backView));                             /* 0x1D392 */
        show_view(D32(D_g_backView));
        ilbm_to_view(D32(D_g_loadedBuf), D32(D_g_backView));         /* the other View gets the cockpit too */
        free_mem(D32(D_g_loadedBuf));
    } else {
        sprites_init();
        init_clouds();
        reset_wheel_state(1);
        setup_drive_copper(D32(D_g_backView));
        render_frame(D32(D_g_backView));
        show_view(D32(D_g_backView));
    }
    setup_drive_copper(D32(D_g_backView));
    render_frame(D32(D_g_backView));
    road_drawer_start();                                             /* 0x24E94: starts paused (D:24AE = 1) */
}

/* keep = 1 between lives and stages (the loaded files stay), 0 at the end of the game. */
static void stage_unload(s16 keep)
{
    vbl_gate_select = 0;                                             /* nothing to shift outside the drive */
    drawer_stop();                                                   /* 0x24F3A */
    reset_wheel_state(1);                                            /* 0x1ED8A */
    sprites_free();                                                  /* 0x1F99A */
    wr16(rd32(DADDR(D_g_viewA) + VIEW_ViewPort) + VP_DyOffset, 0);   /* the crash shake's offset */
    wr16(rd32(DADDR(D_g_viewB) + VIEW_ViewPort) + VP_DyOffset, 0);
    fade_out_drive();                                                /* 0x24E52 */
    if (keep) return;
    if (D32(D_dash_shapes)) free_mem(D32(D_dash_shapes));
    SETD32(D_dash_shapes, 0);
    if (D32(D_road_shapes)) free_mem(D32(D_road_shapes));
    SETD32(D_road_shapes, 0);
    if (D32(D_mask_pool)) free_mem(D32(D_mask_pool));
    SETD32(D_mask_pool, 0);
    if (D32(D_g_carRecord)) free_mem(D32(D_g_carRecord));
    SETD32(D_g_carRecord, 0);
}

/* ================================================================ run_game 0x1C900 */

s16 run_game(s16 car)
{
    const s32 frameLen = 5;              /* -0x10(a5): vertical blanks per drive frame */
    s32 dist = 0, frames = 1, score;
    s16 started = 0, r = 0, secs, limit, n, i;
    u32 t_frame;

    SETD16(D_lane_hold, D16(D_demo_mode));           /* D:0D72 */
    SETD16(D_autopilot, D16(D_demo_mode));           /* D:0D70 */
    sfx_load_drive();                                /* 0x26B7A */
    SETD16(D_dash_palette_valid, 0);                 /* D:2846: re-capture the cockpit palette this game */
    SETD16(D_g_lives, 5);
    SETD16(D_g_tooSlow, 0);
    SETD32(D_g_totalScore, 0);
    SETD16(D_g_stage, D16(D_demo_mode) ? 3 : 0);     /* attract drives stage index 3 */
    SETD16(D_on_enable, 1);                          /* D:284A */
    SETD16(D_sd_enable, 1);                          /* D:2848 */

STAGE:                                               /* 0x1C94A */
    SETD32(D_road_pos, 0x1E0000);                    /* D:28AE = 30.0 road units */
    dist = 0;
    frames = 1;
    stage_road_select(DS16(D_g_stage));              /* 0x20E70: road stream, par, limit, score factor */

LIFE:                                                /* 0x1C968 */
    traffic_reset();                                 /* 0x1CCF6 */
    SETD16(D_near_end, 0);                           /* D:0B2E */
    started = 0;
    r = 0;
    SETD16(D_pedal, 0);                              /* D:1922 */
    SETD16(D_crash, 0);                              /* D:282E */
    SETD32(D_advance, 0);                            /* D:28B6 */
    SETD32(D_speed, 0);                              /* D:28A6 */
    SETD32(D_rpm, 0);                                /* D:191A: rpm word and its fraction */
    SETD16(D_needle_rpm, 0xFFFF);                    /* D:1900 */
    SETD16(D_gear, 0);                               /* D:191E */
    SETD16(D_gate_row, 0);                           /* D:03EE */
    SETD16(D_gate_col, 1);                           /* D:03F0 */
    SETD32(D_road_pos_done, 0xFFFFFFFFu);            /* D:28CE */
    SETD16(D_pitch_prev, 0);                         /* D:283E */
    SETD16(D_pitch_bump_flag, 0);                    /* D:283C */
    SETD16(D_pitch_rate, 0);                         /* D:283A */
    stage_load(car);                                 /* 0x24ACA */
    shift_gear(0);                                   /* 0x1EDCE: the gear knob at its neutral position */
    SETD16(D_knob_x, D16(D_knob_target_x));          /* D:24B8 = D:24B6 */
    SETD16(D_knob_y, D16(D_knob_target_y));          /* D:24B4 = D:24B2 */
    SETD16(D_knob_shown_y, 0);                       /* D:2834 */
    SETD16(D_knob_shown_x, 0);                       /* D:2832 */
    SETD16(D_drawer_pause, 0);                       /* D:24AE: the Road Drawer starts drawing */
    vbl_gate_select = sim_gate_shift_vbl;            /* 0x118B2's O-mode branch calls 0x1EE76 + 0x20826 */
    song_stop();                                     /* e.g. TestGas, still playing from the gas station */
    engine_sound_start();                            /* 0x26C70 */
    /* -0x14(a5) = tick_count and -0x1C(a5) = D:034C are saved here and never used again */
    SETD32(D_drawer_frames, 0);                      /* D:28D6 */
    t_frame = D32(D_tick_count);

    for (;;) {                                       /* 0x1C9FC: one drive frame */
        if (DS16(D_cop_tail_frames) < 0x46) {        /* D:0D76 */
            SETD16(D_autopilot, (D16(D_demo_mode) || D16(D_near_end)) ? 1 : 0);
            SETD16(D_lane_hold, D16(D_autopilot));
        } else {
            SETD16(D_lane_hold, 1);                  /* pulled over by the police: hold the lane */
        }
        if (D16(D_ticket_frames) == 0) sim_controls();       /* D:0D74; 0x20CB0 */
        road_advance();                                      /* 0x1E53A */
        if (D16(D_gear)) started = 1;                        /* the clock starts at the first gear engaged */
        if (started) {
            dist += (s32)DS32(D_speed) >> 16;                /* asr.l #16: mph */
            frames++;
        }
        traffic_update((s16)((s32)DS32(D_road_pos) >> 16));  /* 0x1DDEA */

        /* 0x1CA6A: wait until 5 vertical blanks have passed since the frame started; a slow frame is not
         * caught up. n is a word, so a very late frame simply waits nothing. */
        n = (s16)(u16)(t_frame + (u32)frameLen - D32(D_tick_count));
        for (i = 0; i < n; i++) drive_tick();
        t_frame = D32(D_tick_count);

        knob_update();                                       /* 0x1CD72 */
        /* 0x10CEC: an empty function, called every frame */
        if (quit_requested()) { r = 1; break; }
        poll_input();                                        /* 0x10462 */
        SETD16(D_near_end,
               ((s32)DS16(D_stage_end) - ((s32)DS32(D_road_pos) >> 16)) < 0x50 ? 1 : 0);   /* D:0B2E */
        radar_zone((s32)DS32(D_road_pos));                   /* 0x1EC1A */
        if (D16(D_no_crash) && D16(D_crash)) SETD16(D_crash, 0);          /* D:2830 cancels the crash */
        if (((s32)DS32(D_road_pos) >> 16) >= (s32)DS16(D_stage_end)) break;
        if (D16(D_crash)) break;
        if (D16(D_g_abortKey)) break;                        /* D:0346: Ctrl-R */
        if (D16(D_demo_mode)) {
            if (joy_fire()) break;
            if (ldiv_68k(frames, 0x226) != 0) break;         /* the attract run ends after 550 frames */
        }
        if ((s32)DS32(D_speed) > 0x110000) continue;         /* above 17 mph: keep driving */
        if (D16(D_near_end)) break;                          /* stopped in the finishing zone */
    }

    engine_sound_fade_out();                                 /* 0x26D16 */
    /* -0x18(a5) = tick_count and -0x20(a5) = D:034C are saved here and never used again */
    if (D16(D_g_abortKey) || D16(D_demo_mode) || quit_requested()) goto CLEANUP;

    if (D16(D_crash)) {                                      /* 0x1CB6A */
        frames += 0xF0;                                      /* 240 frames = 20 s penalty */
        drawer_stop();                                       /* 0x24F3A */
        SETD16(D_g_lives, DS16(D_g_lives) - 1);
        if (DS16(D_crash) > 1) SETD16(D_g_lives, 0);         /* a rear-end hit on the police car is fatal */
        crash_sequence();                                    /* 0x1FBD8 */
        if (DS16(D_g_lives) <= 0) {                          /* GAME OVER on the hidden View, then shown */
            APTR shape = D32(D_spr_govr), mask = D32(D_spr_gvrm);   /* D:2766 / D:276A */
            set_clip_full();                                 /* 0x10FA4 */
            gfx_OwnBlitter();
            blit_set_dest(view_planes(D32(D_g_backView)));   /* 0x10E26 */
            blit_shape(shape, mask, (s16)rd16(shape + 8), (s16)rd16(shape + 0xA));   /* 0x10E58 */
            blit_wait();
            gfx_DisownBlitter();
            show_view(D32(D_g_backView));
        }
        wait_for_fire();
        if (DS16(D_g_lives) > 0) {
            stage_unload(1);
            goto LIFE;
        }
        /* game over: the tests below send it to CLEANUP with r = 0 */
    } else {
        stage_unload(1);                                     /* 0x1CC16 */
    }

    if (!D16(D_near_end) || D16(D_crash)) goto CLEANUP;      /* 0x1CC20 */

    score = stage_score(frames, (s32)DS32(D_g_parTime), DS16(D_g_scoreFactor));   /* 0x1D560 */
    SETD32(D_g_totalScore, (s32)DS32(D_g_totalScore) + score);
    free_mem(D32(D_mask_pool));                              /* D:276E: the road shapes' mask arena */
    SETD32(D_mask_pool, 0);

    if (DS16(D_g_stage) >= 4) {                              /* 0x1CCC8: the last stage was driven */
        drawer_stop();                                       /* already stopped by stage_unload */
        song_play_endsuccess();                              /* 0x12EBE */
        dealership_ending();                                 /* 0x1CF76 */
        goto CLEANUP;
    }

    secs  = (s16)ldiv_68k(frames, 0xC);                      /* 12 drive frames = one displayed second */
    limit = (s16)ldiv_68k((s32)DS32(D_g_stageConstB), 0xC);
    stage_results(car, ldiv_68k(dist, frames), secs, score, limit);   /* 0x147E4, the gas station */
    SETD32(D_mask_pool, arena_new(0x7918));                  /* 0x10BA8 */
    load_road_shapes();                                      /* 0x253E6 */
    SETD16(D_g_stage, DS16(D_g_stage) + 1);
    if (!D16(D_g_tooSlow)) goto STAGE;

CLEANUP:                                                     /* 0x1CCD4 */
    if (frames < 0x226) SETD16(D_demo_mode, 0);              /* an attract run that ended early is a real game */
    stage_unload(0);
    sfx_free_drive();                                        /* 0x26C02 */
    return r;                                                /* 1 = Ctrl-C, else 0 (game over) */
}

/* ================================================================ play_again_menu 0x1D5C8 */

/* Two lines in a box; any stick direction toggles the selection (about every 0.4 s), fire chooses.
 * README bug 4: the original's timeout compare is reversed (0x1D876-0x1D886 branches past the timeout unless
 * `t0 > tick_count + 0xE10`, which cannot happen because t0 was read from tick_count before the loop), so the
 * menu waits for ever. Fixed by default: 3600 vertical blanks (60 s) after the last redraw without input
 * return "select a new car" with demo_mode set, so main runs an attract game. */
s16 play_again_menu(void)
{
    static const char SAME[] = "Play again with the same car";
    static const char NEWCAR[] = "      Select a new car      ";
    const s16 w = (s16)(strlen(SAME) * 8 + 0x14);            /* 244 */
    const s16 x0 = (s16)((s16)(0x140 - w) / 2);              /* divs.w: 38 */
    s16 sel = 1, done = 0;
    APTR rp;

    view_clear(D32(D_g_backView));
    view_clear(D32(D_g_frontView));
    for (int k = 0; k < 2; k++) {                            /* the box in pen 3 on both buffers */
        rp = k == 0 ? D32(D_g_backRastPort) : D32(D_g_frontRastPort);
        gfx_SetAPen(rp, 3);
        gfx_RectFill(rp, x0, 0x56, (s16)(x0 + w - 1), 0x71);
        gfx_SetDrMd(rp, 1);                                  /* JAM2 */
    }

    while (!done) {                                          /* 0x1D71E */
        u32 t0 = D32(D_tick_count);
        rp = D32(D_g_backRastPort);
        gfx_SetAPen(rp, 1);
        gfx_SetBPen(rp, (u8)(3 - sel));                      /* the unselected line keeps pen 3 */
        gfx_Move(rp, (s16)(x0 + 0xA), (s16)((s16)rd16(rp + RP_TxBaseline) + 0x5A));
        gfx_Text(rp, SAME, (u16)strlen(SAME));
        gfx_SetBPen(rp, (u8)(sel + 2));                      /* the selected line gets pen 2 (blue) */
        gfx_Move(rp, (s16)(x0 + 0xA), (s16)((s16)rd16(rp + RP_TxBaseline) + 0x66));
        gfx_Text(rp, NEWCAR, (u16)strlen(NEWCAR));
        gfx_LoadRGB4(rd32(D32(D_g_backView) + VIEW_ViewPort), DADDR(D_MENU_PALETTE), 4);
        show_view(D32(D_g_backView));
        dos_Delay(0x14);                                     /* 20 ticks: the toggle rate */
        for (;;) {                                           /* 0x1D850 */
            if (joy_dir() != 0) break;                       /* any direction toggles */
            done = (joy_fire() || quit_requested()) ? 1 : 0;
            if (done) break;
            if (g_original_bugs ? (t0 > D32(D_tick_count) + 0xE10)       /* the dead compare */
                                : (D32(D_tick_count) > t0 + 0xE10)) {    /* the compare it intends */
                done = 1;
                SETD16(D_demo_mode, 1);
                sel = 0;
                break;
            }
            gfx_WaitTOF();
            poll_input();
        }
        if (!done) sel = (s16)(1 - sel);
    }

    rp = D32(D_g_frontRastPort);
    gfx_SetAPen(rp, 0);
    gfx_RectFill(rp, x0, 0x56, (s16)(x0 + w - 1), 0x71);
    return sel;                                              /* 1 = same car, 0 = new car */
}
