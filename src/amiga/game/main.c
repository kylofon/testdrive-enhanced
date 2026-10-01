/* main() — port of td 0x10018 (port/amiga/spec/game_flow.md §4 main; the platform calls in platform_video.md
 * §4.1), and high_scores 0x12F28 (the SCORES state; the table, name entry, file and credits are in
 * game/scores.c).
 *
 * The port always behaves as when started by the disk's Startup-Sequence, `td >nil: p`: argument "p", so the
 * input handler passes every key to the game (passmask 0) and the flow starts at the title. Dropped as in the
 * spec (§5): SetTaskPri / pr_WindowPtr, the CIA-B DDRA/PRA writes, closing the CLI window and Workbench, the
 * copy protection check (always passes), the dead $BFF096 blitter-priority writes, the sprite DMA and sprite
 * data writes (no sprites are shown outside the drive), and the overlay unloads (everything is resident). */
#include "game.h"

#include "../agfx.h"
#include "../ahost.h"
#include "../amem.h"
#include "../asymbols.h"
#include "../../keybind.h"
#include "../paula.h"
#include "../platform/audio.h"
#include "../platform/blit.h"
#include "../platform/platform.h"

/* 0x12F28: load the table, name entry if the last game qualifies, show the table, save it if changed, then the
 * credits when no car has been played yet (or in attract mode). Returns 0, or -1 on a timeout. */
s16 high_scores(s32 score, s16 car)
{
    /* the original's stack frame: names[8][40] -0x140, cars[8][40] -0x280, scores[8] -0x2A0 */
    APTR frame = exec_AllocMem(0x2A0, MEMF_PUBLIC | MEMF_CLEAR);
    if (!frame) ahost_fatal("out of memory (high scores)");
    APTR names = frame + 0x160, cars = frame + 0x20, scores = frame;
    s16 dirty, r = 0;                                   /* r is uninitialised on the early-quit path */
    text_init();                                        /* 0x13250: topaz 8 RastPort D:2876 on bitmap A */
    poll_input();
    if (quit_requested()) goto out;
    dirty = scores_load(names, scores, cars);           /* 1 when the file was missing or bad: rewrite it */
    if (quit_requested()) goto out;
    poll_input();
    if (score > 0 && score > (s32)rd32(scores + 7 * 4)) {   /* signed longs */
        scores_enter_name(score, car, names, scores, cars);
        dirty = 1;                                      /* even if the name was left empty */
    }
    r = scores_show(names, scores, cars);
    if (dirty) scores_save(names, scores, cars);
    if (!quit_requested() && DS16(D_g_selectedCar) < 0) r = credits_show();   /* first run / attract */
out:
    text_shutdown();                                    /* 0x132E6 */
    poll_input();
    exec_FreeMem(frame, 0x2A0);
    return r;
}

/* 0x10018 */
int td_main(void)
{
    gfx_display_init();
    paula_init();

    /* the command line of the Startup-Sequence: "td >nil: p" */
    const int argc = 2;
    const char *const argv1 = "p";

    s16 r, car = 0, inputArg = 0x5C;
    SETD16(D_g_fatalInit, 0);
    SETD16(D_g_selectedCar, -1);
    SETD16(D_Enable_Abort, 0);                         /* D:24AC, set by _main from the CLI */
    SETD32(D_g_totalScore, 0);
    SETD16(D_demo_mode, 0);
    SETD16(D_g_inGame, 0);
    SETD16(D_quit_flag, 0);
    if (argc == 2 && argv1[0] == 'p') inputArg = 0;     /* close_cli_window 0x1032C dropped */
    display_init();                                     /* 0x115BC */
    vbl_install();                                      /* 0x1180E: tick_count */
    input_init((u16)inputArg);                          /* 0x154E4 */
    sfx_init();                                         /* 0x12304 */
    /* copy_protection_check 0x10588: dropped (passes) */
    song_init();                                        /* 0x1278C */
    gfx_WaitTOF();
    /* DMACON = 0x0020 (sprite DMA off): no sprites outside the drive in the port */
    dos_Delay(3);
    /* SPR0DATA/B, SPR1DATA/B = 0: as above */
    fire_port_init();                                   /* 0x153CC */
    /* blitter_pri_save 0x1042E: dead write, dropped */
    car = 0;
    load_cars_txt(1);
    if (D16(D_g_fatalInit) || quit_requested()) goto EXIT;

    if (argc != 2) goto GAME;                           /* also a Workbench start (argc 0) */
    switch (argv1[0]) {
    case 'p':
    case 't': goto TITLE;
    case 'h': goto SCORES;
    case 's': goto SELECT;
    default:  goto GAME;                                /* 'g' and anything else: car 0, not attract */
    }

TITLE:
    song_play_testdrive();
    r = run_intro();
    if (quit_requested()) goto EXIT;
    if (r == -1) goto SCORES;
    SETD16(D_demo_mode, 0);
    song_stop();
    goto SELECT;

SCORES:
    song_play_testdrive();
    r = high_scores((s32)D32(D_g_totalScore), car);
    SETD32(D_g_totalScore, 0);
    if (quit_requested()) goto EXIT;
    SETD16(D_demo_mode, r == -1);
    song_stop();
    /* fall through */
SELECT:
    SETD16(D_g_abortKey, 0);
    if (D16(D_demo_mode) == 0 && DS16(D_g_selectedCar) != -1 && play_again_menu() != 0) {
        r = DS16(D_g_selectedCar);                      /* same car; no song */
    } else {
        song_play_test2();
        load_cars_txt(1);
        r = car_select();
    }
    if (quit_requested()) goto EXIT;
    if (D16(D_g_abortKey)) {
        SETD16(D_demo_mode, 1);
        song_stop();
        goto TITLE;
    }
    if (r == -1) {
        SETD16(D_demo_mode, 1);
        /* divu: unsigned; a count of 0 would be a divide-by-zero trap on the 68000 */
        r = D16(D_g_numCars) ? (s16)((u16)rand16() % D16(D_g_numCars)) : 0;
    } else {
        SETD16(D_demo_mode, 0);
    }
    car = r;
    if (car < 0 || car > DS16(D_g_numCars)) {           /* '>' not '>=' (unreachable) */
        SETD16(D_demo_mode, 0);
        goto SCORES;
    }

GAME:
    SETD16(D_g_abortKey, 0);
    /* overlay_unload(stub 0x1AED0): nothing to do */
    SETD32(D_g_totalScore, 0);
    SETD16(D_g_inGame, 1);
    SETD16(D_g_selectedCar, D16(D_demo_mode) ? -1 : car);
    kb_set_active(true);                                /* PORT: key bindings while driving */
    r = run_game(car);
    kb_set_active(false);
    SETD16(D_g_inGame, 0);
    /* overlay_unload(stub 0x1C900): nothing to do */
    if (quit_requested()) goto EXIT;
    if (D16(D_g_abortKey)) goto SELECT;
    if (D16(D_demo_mode)) goto TITLE;
    if (r == -1) goto TITLE;                            /* run_game never returns -1 */
    if (r == 0) goto SCORES;
    /* r == 1 (Ctrl-C inside run_game): fall into EXIT */

EXIT:
    song_stop();
    load_cars_txt(0);                                   /* clears the count only */
    /* blitter_pri_restore 0x1044C and DMACON = 0x8020 (sprite DMA on): dropped */
    song_stop();
    song_shutdown();
    sfx_shutdown();
    input_shutdown();
    vbl_remove();
    display_shutdown();
    /* close_libs 0x115A0: nothing to do */
    free_mem(0xFFFFFFFFu);                              /* every tracked allocation */
    return 0;                                           /* the original's return value is not set */
}
