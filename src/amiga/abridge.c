/* The Amiga engine as the DOS game uses it: see abridge.h. The start-up follows td's main() (game/main.c,
 * td_main), whose own flow is not used: the DOS game's flow calls the parts it wants. */
#include "abridge.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../amiga_opts.h"
#include "adisk.h"
#include "agfx.h"
#include "ahost.h"
#include "amem.h"
#include "aport.h"
#include "asymbols.h"
#include "game/game.h"
#include "paula.h"
#include "platform/audio.h"
#include "platform/platform.h"

bool g_original_bugs;           /* the port's fixes of the Amiga original's bugs are always applied here */

static bool ready;
static char car_files[16][32];

bool amiga_ready(void) { return ready; }

bool amiga_open(char *err, size_t errlen)
{
    if (ready) return true;
    u32 len = 0;
    u8 *td = NULL;
    bool ok = adisk_open(amiga_disk, err, errlen);
    if (ok && !(td = adisk_read("td", &len))) {
        snprintf(err, errlen, "%s has no td (the Amiga game's program).", adisk_source());
        ok = false;
    }
    ok = ok && amem_load_exe(td, len, err, errlen);
    free(td);
    if (!ok) return false;

    ahost_init(0);
    gfx_display_init();
    paula_init();
    SETD16(D_g_fatalInit, 0);
    SETD16(D_g_selectedCar, -1);
    SETD16(D_Enable_Abort, 0);
    SETD32(D_g_totalScore, 0);
    SETD16(D_demo_mode, 0);
    SETD16(D_g_inGame, 0);
    SETD16(D_quit_flag, 0);
    display_init();                                     /* 0x115BC */
    vbl_install();                                      /* 0x1180E: tick_count */
    input_init(0);                                      /* "td >nil: p": every key to the game */
    sfx_init();
    song_init();
    gfx_WaitTOF();
    fire_port_init();
    load_cars_txt(1);
    if (D16(D_g_fatalInit)) {
        snprintf(err, errlen, "%s: Cars.txt is missing or unreadable.", adisk_source());
        return false;
    }
    for (int i = 0; i < DS16(D_g_numCars) && i < 16; i++)
        amem_cstr(D32(D_g_carNames + 4 * (u32)i), car_files[i], sizeof car_files[i]);
    ready = true;
    return true;
}

void amiga_close(void)
{
    if (!ready) return;
    amiga_images_free();
    ready = false;
    song_stop();
    song_shutdown();
    sfx_shutdown();
    input_shutdown();
    vbl_remove();
    display_shutdown();
    ahost_shutdown();
}

int amiga_intro(void)
{
    amiga_screens_begin();
    SETD16(D_g_abortKey, 0);
    s16 r = run_intro();                                /* -1 ran to the end, 1 interrupted */
    SETD16(D_demo_mode, 0);
    amiga_screens_end();
    return r == -1 ? -1 : 0;
}

int amiga_car_select(bool demo)
{
    amiga_screens_begin();
    SETD16(D_g_abortKey, 0);
    SETD16(D_demo_mode, demo ? 1 : 0);
    load_cars_txt(1);
    s16 r = car_select();
    bool abort = D16(D_g_abortKey) != 0;
    SETD16(D_g_abortKey, 0);
    SETD16(D_demo_mode, 0);
    amiga_screens_end();
    return abort ? -2 : r;
}

int amiga_play_again(void)
{
    amiga_screens_begin();
    s16 r = play_again_menu();
    amiga_screens_end();
    return r;
}

void amiga_stage_results(s16 car, s16 stage, s32 avgSpeed, s16 secs, s32 score, s16 limit)
{
    amiga_screens_begin();
    drive_stage_results(car, stage, avgSpeed, secs, score, limit);
    amiga_screens_end();
}

const char *amiga_car_file(int idx)
{
    return ready && idx >= 0 && idx < DS16(D_g_numCars) && idx < 16 ? car_files[idx] : "";
}

int amiga_car_count(void) { return ready ? DS16(D_g_numCars) : 0; }

void amiga_song_play(int song)
{
    if (!ready) return;
    switch (song) {
    case AMIGA_SONG_TESTDRIVE:  song_play_testdrive(); break;
    case AMIGA_SONG_TEST2:      song_play_test2(); break;
    case AMIGA_SONG_TESTGAS:    song_play_testgas(); break;
    case AMIGA_SONG_ENDSUCCESS: song_play_endsuccess(); break;
    default: break;
    }
}

void amiga_song_stop(void)
{
    if (ready) song_stop();
}

bool amiga_song_playing(void) { return ready && song_is_active() != 0; }
