/* The drive's sounds — port of td 0x26B7A-0x26EE2 (port/amiga/spec/platform_audio.md §4.13, §4.14, §4.17):
 * the five drive samples, the engine/turbo/squeal loops on channels 3/2/1 and the bump and radar one-shots on
 * channel 0. The loops are started once per life by engine_sound_start and updated once per drive frame by
 * engine_sound_update (called from drive_sim's 0x20826); everything else about the channels is the sfx engine
 * in audio.c.
 *
 * Original bugs owned here (port/amiga/README.md, behind g_original_bugs; fixed by default):
 *   bug 8 — with sound off at stage start the original still starts the squeal and turbo loops (only the engine
 *           fade-in is gated at 0x26CF0), so they are audible with `S` off; fixed: start no loop at all, which
 *           is the state the game itself reaches when `S` is pressed mid-drive.
 *   bug 9 — the three loops are started at the sample header instead of +6 with the same length (0x26C8A,
 *           0x26CAA, 0x26CD4), so every loop pass plays the 6 header bytes (a click) and drops the last 6
 *           samples; fixed: start at +6, as play_sample 0x1246A does.
 */
#include "audio_drive.h"

#include "audio.h"
#include "platform.h"
#include "../aport.h"
#include "../asymbols.h"

/* D:192A has no name in asymbols.h: the engine-sound flag copied from the car record +006 (0 = the
 * non-turbo engine sound, one octave down with a +1500 rpm offset; FORMATS.md "Car record"). */
#define D_car_sound_flag  0x192A

/* Channel record of the sfx engine (D:0446 + 0x1E * ch); only the data pointer is read here. */
#define CH_DATA  0x00
static APTR sfx_chan(u16 ch) { return DADDR(D_sfx_chan + (u32)ch * 0x1E); }

/* 68000 divu.w: 32/16 unsigned, quotient in the low word; on overflow the destination is left unchanged. */
static u16 divu_w(u32 dividend, u16 divisor)
{
    if (divisor == 0) return (u16)dividend;      /* would trap on the 68000; rpm is floored at 0x320 below */
    u32 q = dividend / divisor;
    return q > 0xFFFFu ? (u16)dividend : (u16)q;
}

/* Sample file: u32 length, u16 rate, then the 8-bit signed data (FORMATS.md, Amiga "Sfx"). */
static u32 sample_len(APTR s) { return s ? rd32(s) : 0; }

/* §4.13 warns that sfx_free_drive frees the samples while the channels may still be playing them (the
 * original relies on Paula reading freed memory until the next start). The port detaches first: any channel
 * still reading inside the block is stopped. Channels playing something else (the ending song, which
 * song_play_endsuccess has just started on 0-2) are left alone. */
static void detach_channels(APTR sample)
{
    if (!sample) return;
    u32 lo = sample, hi = sample + 6 + sample_len(sample);
    for (s16 ch = 0; ch < 4; ch++) {
        u32 p = rd32(sfx_chan((u16)ch) + CH_DATA);
        if (p >= lo && p < hi) sfx_stop_channel(ch);
    }
}

static void free_sample(u16 slot)                /* one of D:1BAE..D:1BBE */
{
    APTR p = D32(slot);
    detach_channels(p);
    free_mem(p);                                 /* 0x159FA */
    SETD32(slot, 0);
}

/* ---------------------------------------------------------------- 0x26B7A / 0x26C02 */

void sfx_load_drive(void)                        /* 0x26B7A */
{
    SETD32(D_sfx_radar,  load_file_chip("sfx/Radar"));       /* D:1BAE */
    SETD32(D_sfx_engine, load_file_chip("sfx/TheEngine"));   /* D:1BB2 */
    SETD32(D_sfx_turbo,  load_file_chip("sfx/TheTurbo"));    /* D:1BB6 */
    SETD32(D_sfx_squeal, load_file_chip("sfx/Squeal"));      /* D:1BBA */
    SETD32(D_sfx_bump,   load_file_chip("sfx/Bump"));        /* D:1BBE */
}

void sfx_free_drive(void)                        /* 0x26C02 */
{
    free_sample(D_sfx_bump);
    free_sample(D_sfx_squeal);
    free_sample(D_sfx_turbo);
    free_sample(D_sfx_engine);
    free_sample(D_sfx_radar);
}

/* ---------------------------------------------------------------- 0x26C50 / 0x26C70 / 0x26D16 */

void radar_beep(void)                            /* 0x26C50 */
{
    if (D16(D_sfx_enabled)) play_sample(D32(D_sfx_radar), 0, 0x40);   /* D:0344 */
}

void engine_sound_start(void)                    /* 0x26C70 */
{
    sfx_set_song_channels(0);                    /* 0x12432: all four channels are the drive's */

    /* Bug 8: the original starts the loops whatever D:0344 says and gates only the fade-in below. */
    if (!D16(D_sfx_enabled) && !g_original_bugs) return;

    /* Bug 9: the original passes the sample header with the full length; the fix skips the 6-byte header. */
    u32 h = g_original_bugs ? 0 : 6;
    APTR squeal = D32(D_sfx_squeal), turbo = D32(D_sfx_turbo), engine = D32(D_sfx_engine);

    start_channel(squeal + h, sample_len(squeal), 1, 0x166, 0, -1);   /* 0x26C7C: squeal, native ~10 kHz */
    start_channel(turbo  + h, sample_len(turbo),  2, 0x320, 0, -1);   /* 0x26C9C: the turbo layer */
    dos_Delay(2);                                                     /* 0x26CBC: the sfx server needs 2 ticks */
    start_channel(engine + h, sample_len(engine), 3, 0x320, 0, -1);   /* 0x26CC6: the engine */
    dos_Delay(2);                                                     /* 0x26CE6 */
    if (D16(D_sfx_enabled)) {                                         /* 0x26CF0 */
        sfx_slide_volume(3, 0x3F, 0x7000);       /* 0x3F0000 / 0x7000 = 144 ticks = 2.4 s fade-in */
        SETD16(D_skid_amount, 0);                /* D:19BC */
        SETD16(D_squeal_env, 0xFFFF);            /* D:15FE */
    }
}

void engine_sound_fade_out(void)                 /* 0x26D16: stage end, crash, quit */
{
    sfx_slide_volume(1, 0, 0x7000);
    sfx_slide_volume(2, 0, 0x7000);
    sfx_slide_volume(3, 0, 0x7000);
}

/* ---------------------------------------------------------------- 0x26E26 / 0x26EE2 */

/* The chirp envelope, one entry per drive frame (tables 0x26E7E and 0x26EB0; entry 0 is never used: 0x207A4
 * sets D:15FE to 0 or 0x0B and squeal_update increments before indexing). */
static const u16 SQUEAL_VOLUME[25] = {          /* 0x26E7E */
    0x0040, 0x0040, 0x0040, 0x0040, 0x0040, 0x0040, 0x0040, 0x0040,
    0x0040, 0x0040, 0x0040, 0x0040, 0x0040, 0x0040, 0x0040, 0x0040,
    0x0040, 0x0040, 0x003C, 0x0032, 0x002D, 0x0020, 0x0014, 0x000A,
    0x0000,
};
static const u16 SQUEAL_PERIOD[25] = {          /* 0x26EB0 */
    0x015C, 0x015C, 0x015C, 0x0172, 0x017C, 0x0182, 0x0188, 0x018B,
    0x0186, 0x0186, 0x0181, 0x0181, 0x0180, 0x017F, 0x017E, 0x017D,
    0x017A, 0x0177, 0x0172, 0x0168, 0x015E, 0x0154, 0x014F, 0x014F,
    0x014F,
};

/* 0x26E26: the tyre squeal on channel 1 — either one step of the wheelspin chirp or the steady skid tone
 * whose volume is the grip excess D:19BC that 0x24A04 computes every frame. */
static void squeal_update(void)
{
    s16 env;
    if (D16(D_lane_hold)) {                      /* D:0D72: demo / finishing zone / police tail: silent */
        SETD16(D_skid_amount, 0);                /* 0x26E60 */
        goto steady;
    }
    env = DS16(D_squeal_env);                    /* D:15FE, -1 = no chirp */
    if (env < 0) goto steady;
    env = (s16)(env + 1);
    SETD16(D_squeal_env, env);
    if (env >= 0x19) goto steady;
    sfx_set_period_vol(1, (s16)SQUEAL_PERIOD[env], (s16)SQUEAL_VOLUME[env]);
    return;
steady:                                          /* 0x26E64 */
    SETD16(D_squeal_env, 0xFFFF);
    sfx_set_period_vol(1, 0x166, DS16(D_skid_amount));
}

/* 0x26EE2: one bump sample on channel 0 for a road-edge marker (D:0D9A) or a pothole/bump hazard (D:19BE).
 * A request is dropped when channel 0 is still busy. */
static void bump_update(void)
{
    if ((D16(D_bump_flag) || D16(D_bump_edge)) && !sfx_channel_busy(0) && D16(D_sfx_enabled))
        play_sample(D32(D_sfx_bump), 0, 0x40);
    SETD16(D_bump_flag, 0);                      /* D:0D9A */
    SETD16(D_bump_edge, 0);                      /* D:19BE */
}

/* ---------------------------------------------------------------- 0x26D4E */

/* Once per drive frame, from the end of the engine/gearbox update 0x20826. Channel 3 plays the engine loop at
 * a period taken from the rpm, channel 2 the same engine sample as a "turbo" layer whose volume follows the
 * rpm above 2000 and the speed above 90 mph. */
void engine_sound_update(void)
{
    squeal_update();                             /* 0x26E26 */
    bump_update();                               /* 0x26EE2 */

    s16 rpm = DS16(D_rpm);                       /* D:191A */
    if (rpm < 0x320) rpm = 0x320;                /* 800 rpm floor */
    s16 per = (s16)divu_w(0x249988u, (u16)rpm);  /* 2398600 / rpm */
    if (per < 0x82)  per = 0x82;
    if (per > 0x708) per = 0x708;                /* reached below 1333 rpm */
    SETD16(D_engine_period, per);                /* D:1BC2 */
    sfx_set_period_vol(3, per, -1);

    s16 vol = 0;
    if (DS16(D_pedal) < 0) goto out;             /* D:1922 < 0: braking, the turbo layer is silent */
    if (DS16(D_pedal) > 0) {
        if (D16(D_gear) == 0) goto out;          /* D:191E */
        s16 r = DS16(D_rpm);
        if (D16(D_car_sound_flag) == 0) r = (s16)(r + 0x5DC);   /* D:192A car record +006: +1500 for non-turbo cars */
        if (r <= 0x7D0) goto out;                /* 2000 */
        if (r > 0xFA0) r = 0xFA0;                /* 4000 */
        vol = (s16)((((u32)(u16)(r - 0x7D0) * 0x831u) >> 16) & 0xFFFFu);   /* mulu.w + swap: 0..63 */
    }
    {
        s16 w = (s16)(DS16(D_speed) - 0x5A);     /* the high word of D:28A6 = mph, above 90 */
        if (w <= 0) goto out;
        if (w >= 0x20) w = 0x20;
        vol = (s16)(vol + w);
        if (vol > 0x40) vol = 0x40;
    }
out:                                             /* 0x26DF2 */
    sfx_slide_volume(2, vol, 0x10000);           /* one volume step per vertical blank */
    s16 per2 = DS16(D_engine_period);
    if (D16(D_car_sound_flag) == 0) per2 = (s16)(per2 << 1);   /* one octave down for the non-turbo cars */
    sfx_set_period_vol(2, per2, -1);
}
