/* AMIGA: the DOS game's flow with the Amiga features (--amiga, ../amiga_opts.h): the Amiga engine (../amiga) runs
 * the title sequence, the car selection and the play-again menu in place of the DOS ones, and plays its songs in
 * place of the speaker's. Not bound by the faithful-port rules (ENGINE.md), like src/enhanced. */
#include "flow.h"

#include <ctype.h>
#include <string.h>

#include "../amiga_opts.h"
#include "../amiga/abridge.h"
#include "../host.h"
#include "../platform/input.h"
#include "../platform/res.h"
#include "../platform/timer.h"

void flow_amiga_start(void)
{
    if (!amiga_any()) return;
    char err[512];
    if (!amiga_open(err, sizeof err))
        host_fatal("The Amiga features need the Amiga disk of Test Drive.\n\n%s", err);
}

/* The DOS car with the Amiga car's name: Cars.txt names "cars/P911t", CARS.TXT "p911t" (8.3: "counta" for
 * "Countach"). -1 if there is none. */
static int dos_car_of(int amiga_idx)
{
    const char *file = amiga_car_file(amiga_idx);
    const char *base = strrchr(file, '/');
    base = base ? base + 1 : file;
    for (int i = 0; i < DSS(DS_g_numCars); i++) {
        const char *dos = flow_car_name(i);
        size_t n = strlen(dos);
        size_t k = 0;
        while (k < n && base[k] && tolower((unsigned char)dos[k]) == tolower((unsigned char)base[k])) k++;
        if (n && k == n) return i;
    }
    return -1;
}

/* The songs: the DOS one, or the Amiga one in its place (Amiga music). */
void flow_song_play(u16 ds_song)
{
    if (amiga.music) {
        int s = ds_song == DS_g_songIntro || ds_song == DS_g_songHiScore ? AMIGA_SONG_TESTDRIVE
              : ds_song == DS_g_songCarSel ? AMIGA_SONG_TEST2
              : ds_song == DS_g_songGas ? AMIGA_SONG_TESTGAS : -1;
        if (s >= 0) {
            amiga_song_play(s);
            return;
        }
    }
    snd_play_oneshot(far_rd(DGROUP, ds_song));
}

void flow_song_stop(void)
{
    if (amiga.music) amiga_song_stop();
    snd_stop_oneshot();
}

void flow_ending_song(bool on)
{
    if (!amiga.music) return;
    if (on) amiga_song_play(AMIGA_SONG_ENDSUCCESS);
    else amiga_song_stop();
}

/* run_intro's results: -1 ran to the end (attract: the scores next), 1 Esc (back to DOS?), else a key. */
int flow_amiga_intro(void)
{
    flow_song_play(DS_g_songIntro);
    int r = amiga_intro();
    flow_song_stop();
    kbd_flush();
    return r;
}

/* car_select's results: 0 chosen, -1 the timeout (attract mode, a car is picked), 1 Esc (back to DOS?). */
int flow_amiga_car_select(void)
{
    flow_song_play(DS_g_songCarSel);
    int r = amiga_car_select(DSW(DS_demo_mode) != 0);
    flow_song_stop();
    kbd_flush();
    if (r == -2) return 1;                                /* Ctrl-R: the Amiga goes back to the title */
    int car = r >= 0 ? dos_car_of(r) : -1;
    if (car < 0) {                                        /* attract mode, or a car the DOS game hasn't */
        car = DSS(DS_g_numCars) > 0 ? crt_rand() % DSS(DS_g_numCars) : 0;
        DSS(DS_g_selectedCar) = (s16)car;
        DSW(DS_g_carAutoSelected) = 1;
        return -1;
    }
    DSS(DS_g_selectedCar) = (s16)car;
    DSW(DS_g_carAutoSelected) = 0;
    return 0;
}

/* play_again_menu's results: 0 the same car, 1 a new one. */
int flow_amiga_play_again(void)
{
    int r = amiga_play_again();
    kbd_flush();
    return r == 1 ? 0 : 1;
}
