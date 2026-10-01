#pragma once
/* Sound effects engine and SMUS song player — port of td 0x12304-0x12EF6 (port/amiga/spec/platform_audio.md
 * §4.1-§4.12, §4.16-§4.17). Runs on the Paula emulation (../paula.h); the servers are VBL servers of the host
 * (Song pri 0x20, Sfx pri 0x1E). The drive sounds (0x26B7A-0x26EE2, overlay 2) come with the drive. */
#include "../amem.h"

void sfx_init(void);                                            /* 0x12304 */
void sfx_shutdown(void);                                        /* 0x123EC */
void sfx_set_song_channels(u16 mask);                           /* 0x12432 */
void play_sample(APTR sample, s16 ch, s16 vol);                 /* 0x1246A: sample = file (u32 len, u16 rate, data) */
void start_channel(APTR data, u32 len, s16 ch, s16 period, s16 vol, s16 repeats);   /* 0x124C4 */
void snd_stop_all(void);                                        /* 0x1252A */
void sfx_stop_channel(s16 ch);                                  /* 0x12546 */
s16  sfx_channel_busy(s16 ch);                                  /* 0x1258E: -1 busy, 0 idle */
void sfx_set_period_vol(s16 ch, s16 period, s16 vol);           /* 0x125A6: < 0 = leave */
void sfx_slide_volume(s16 ch, s16 target, s32 step);            /* 0x125F4 */

void song_init(void);                                           /* 0x1278C */
void song_shutdown(void);                                       /* 0x127F4 */
void song_set_volume(s16 vol, s32 step);                        /* 0x12942: step 0 = at once */
s16  song_get_volume(void);                                     /* 0x1295E */
s16  song_is_active(void);                                      /* 0x12A40 */
void song_stop(void);                                           /* 0x12DB0 */
void song_play_testdrive(void);                                 /* 0x12E12 */
void song_play_test2(void);                                     /* 0x12E58 */
void song_play_testgas(void);                                   /* 0x12E8A */
void song_play_endsuccess(void);                                /* 0x12EBE */
void song_play_loser(void);                                     /* 0x12EF6 (no caller in the original) */
