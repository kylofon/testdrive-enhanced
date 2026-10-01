/* Sound effects engine and SMUS song player — port of td 0x12304-0x12EF6 (port/amiga/spec/platform_audio.md
 * §4.1-§4.12, §4.16-§4.17). All state lives in the original globals (D:0446 channel records, D:073A track
 * records, D:1EF0-D:1EF8 song volume, ...); the custom-chip writes go to the Paula emulation. */
#include "audio.h"

#include <stdio.h>
#include <string.h>

#include "platform.h"
#include "../ahost.h"
#include "../aport.h"
#include "../asymbols.h"
#include "../paula.h"

/* ---------------------------------------------------------------- records and helpers */

/* Channel record (D:0446 + 0x1E * ch). */
#define CH_DATA     0x00    /* long: data pointer, 0 = idle */
#define CH_STOP     0x04    /* long: sfx_tick of the last stop */
#define CH_PENDING  0x08    /* int: start pending (-1) */
#define CH_LEN      0x0A    /* int: length in words */
#define CH_PERIOD   0x0C    /* int: period at start */
#define CH_VOL      0x0E    /* long: volume 16.16 */
#define CH_REPEATS  0x12    /* int: repeats (-1 = forever) */
#define CH_LEFT     0x14    /* int: repeats left */
#define CH_TARGET   0x16    /* long: slide target 16.16 (-1 = none) */
#define CH_STEP     0x1A    /* long: slide step 16.16 */

/* Track record (D:073A + 0x18 * k). */
#define TR_ACTIVE   0x00    /* int */
#define TR_INST     0x02    /* long: instrument sample */
#define TR_ONESHOT  0x06    /* int */
#define TR_SIZE     0x08    /* long: TRAK size in bytes */
#define TR_DATA     0x0C    /* long: TRAK data */
#define TR_POS      0x10    /* long: read position */
#define TR_COUNT    0x14    /* int: countdown (ticks) */
#define TR_TIE      0x16    /* int: tie flag */

/* Interrupt structure fields (exec Node + is_Data/is_Code). */
#define IS_TYPE     0x08
#define IS_PRI      0x09
#define IS_NAME     0x0A
#define IS_DATA     0x0E
#define IS_CODE     0x12

#define ADDR_SAVED_A4       0x12612u    /* long in the code hunk, read by the level-4 handler */
#define ADDR_SFX_AUDIO_INT  0x12616u
#define ADDR_SFX_VBL        0x126AEu
#define ADDR_SFX_VBL_NAME   0x123E0u    /* "Sfx VBLInt" */
#define ADDR_SONG_VBL       0x12A54u
#define ADDR_SONG_VBL_NAME  0x127E8u    /* "Song VBLInt" */
#define ADDR_LEVEL4_VECTOR  0x70u

#define SONG_HZ             50          /* PORT: the song player's tick rate (PAL VBL), see song_vbl_server */

static APTR chan(u16 ch) { return DADDR(D_sfx_chan + (u32)ch * 0x1E); }
static APTR track(int k) { return DADDR(D_song_track + (u32)k * 0x18); }

/* Copies a C string out of amem[] (always terminated). */
static void amem_cstr(APTR s, char *out, size_t n)
{
    size_t i = 0;
    for (; i + 1 < n; i++) {
        u8 c = rd8(s + (u32)i);
        if (!c) break;
        out[i] = (char)c;
    }
    out[i] = 0;
}

/* 68000 divu.w: 32/16 unsigned; on overflow the destination is left unchanged (V set). */
static u16 divu_w(u32 dividend, u16 divisor)
{
    if (divisor == 0) return (u16)dividend;     /* would trap on the 68000; never happens with the shipped data */
    u32 q = dividend / divisor;
    return q > 0xFFFFu ? (u16)dividend : (u16)q;
}

static void sfx_vbl_server(void);
static void sfx_audio_int(void);
static void song_vbl_server(void);
static void song_request_stop(void);

/* ---------------------------------------------------------------- sfx engine */

void sfx_init(void)                                     /* 0x12304 */
{
    if (D16(D_sfx_installed)) return;
    sfx_set_song_channels(0);
    wr32(ADDR_SAVED_A4, A4_VALUE);
    SETD32(D_sfx_server_a4, A4_VALUE);
    SETD32(D_sfx_tick, 0);
    for (u16 ch = 0; ch < 4; ch++) {
        wr32(chan(ch) + CH_DATA, 0);
        wr32(chan(ch) + CH_PENDING, 0);                 /* clr.l: pending and length */
        wr32(chan(ch) + CH_TARGET, 0xFFFFFFFFu);
    }
    paula_intena(0x0780);                               /* audio interrupts off */
    paula_dmacon(0x000F);                               /* audio DMA off */
    /* ADKCON = 0x00FF: no modulation, nothing to emulate */
    SETD32(D_old_level4_vector, rd32(ADDR_LEVEL4_VECTOR));
    wr32(ADDR_LEVEL4_VECTOR, ADDR_SFX_AUDIO_INT);
    paula_set_level4_handler(sfx_audio_int);
    u32 t = D32(D_sfx_tick);
    for (u16 ch = 0; ch < 4; ch++) wr32(chan(ch) + CH_STOP, t);
    paula_intena(0x8780);                               /* audio interrupts on */
    SETD8(D_sfx_vbl_interrupt + IS_TYPE, 2);            /* NT_INTERRUPT */
    SETD8(D_sfx_vbl_interrupt + IS_PRI, 0x1E);
    SETD32(D_sfx_vbl_interrupt + IS_NAME, ADDR_SFX_VBL_NAME);
    SETD32(D_sfx_vbl_interrupt + IS_DATA, D32(D_sfx_server_a4));
    SETD32(D_sfx_vbl_interrupt + IS_CODE, ADDR_SFX_VBL);
    ahost_add_vbl_server(0x1E, sfx_vbl_server);         /* AddIntServer(INTB_VERTB, ...) */
    SETD16(D_sfx_installed, 1);
}

void sfx_shutdown(void)                                 /* 0x123EC */
{
    if (D16(D_sfx_installed)) {
        snd_stop_all();
        ahost_delay(5);
        ahost_remove_vbl_server(sfx_vbl_server);
        paula_intena(0x0780);
        paula_dmacon(0x000F);
        wr32(ADDR_LEVEL4_VECTOR, D32(D_old_level4_vector));
        paula_set_level4_handler(NULL);
    }
    SETD16(D_sfx_installed, 0);
}

void sfx_set_song_channels(u16 mask)                    /* 0x12432 */
{
    SETD16(D_sfx_int_channels, (u16)((u16)(~mask & 0xF) << 7));
    if (D16(D_sfx_installed)) {
        u16 v = (u16)(D16(D_sfx_int_channels) | 0x8000);
        paula_intena(v);                                /* enable the sfx channels */
        paula_intena((u16)(v ^ 0x8780));               /* clear the other audio bits */
    }
}

void play_sample(APTR sample, s16 ch, s16 vol)          /* 0x1246A */
{
    if (!sample) return;
    u16 rate = rd16(sample + 4);
    if (rate < 100) rate = (u16)(rate * 1000u);         /* mulu.w, then only the low word is used */
    /* ldiv(0x369E99, rate): 0x369E99 is the NTSC Paula clock. A rate of 0 does not occur in the files. */
    s16 period = rate ? (s16)(u16)(0x369E99u / rate) : 0;
    start_channel(sample + 6, rd32(sample), ch, period, vol, 1);
}

void start_channel(APTR data, u32 len, s16 ch, s16 period, s16 vol, s16 repeats)  /* 0x124C4 */
{
    if (!data) return;
    if (sfx_channel_busy(ch)) sfx_stop_channel(ch);
    APTR c = chan((u16)ch);
    wr16(c + CH_LEN, (u16)(len >> 1));                  /* odd last byte dropped */
    wr16(c + CH_PERIOD, (u16)period);
    wr16(c + CH_VOL, (u16)vol);                         /* 16.16: word at +0E, clr.w +10 */
    wr16(c + CH_VOL + 2, 0);
    wr16(c + CH_REPEATS, (u16)repeats);
    wr32(c + CH_DATA, data);
    wr32(c + CH_TARGET, 0xFFFFFFFFu);
    wr16(c + CH_PENDING, 0xFFFF);                       /* the VBL server starts it */
}

void snd_stop_all(void)                                 /* 0x1252A */
{
    for (s16 ch = 0; ch < 4; ch++) sfx_stop_channel(ch);
}

void sfx_stop_channel(s16 ch)                           /* 0x12546 */
{
    u16 n = (u16)ch;
    APTR c = chan(n);
    wr16(c + CH_PENDING, 0);
    paula_dmacon((u16)(1u << (n & 15)));                /* clear */
    wr32(c + CH_TARGET, 0xFFFFFFFFu);
    paula_set_vol(n, 0);
    wr32(c + CH_STOP, D32(D_sfx_tick));
    wr32(c + CH_DATA, 0);
}

s16 sfx_channel_busy(s16 ch)                            /* 0x1258E */
{
    return rd32(chan((u16)ch) + CH_DATA) ? -1 : 0;
}

void sfx_set_period_vol(s16 ch, s16 period, s16 vol)    /* 0x125A6 */
{
    APTR c = chan((u16)ch);
    if (period >= 0) paula_set_per(ch, (u16)period);    /* the record's +0C is not updated */
    if (vol >= 0) {
        wr32(c + CH_TARGET, 0xFFFFFFFFu);               /* cancels a slide */
        wr16(c + CH_VOL, (u16)vol);
        wr16(c + CH_VOL + 2, 0);
        paula_set_vol(ch, (u16)vol);
    }
}

void sfx_slide_volume(s16 ch, s16 target, s32 step)     /* 0x125F4 */
{
    APTR c = chan((u16)ch);
    wr32(c + CH_STEP, (u32)step);
    wr32(c + CH_TARGET, (u32)(u16)target << 16);        /* move.l 6(a7),d1 ; clr.w d1 */
}

/* 0x12616: the level-4 autovector. Called by Paula at the sample where an enabled channel (re)loads. */
static void sfx_audio_int(void)
{
    u16 pend = (u16)(paula_intreqr() & paula_intenar() & 0x0780);
    u16 mine = D16(D_sfx_int_channels);
    paula_intreq((u16)(pend & ~mine));                  /* acknowledge the song channels' requests */
    pend &= mine;
    for (u16 ch = 0; ch < 4; ch++) {
        u16 bit = (u16)(0x80 << ch);
        if (!(pend & bit)) continue;
        APTR c = chan(ch);
        bool stop;
        if (rd32(c + CH_DATA) == 0) stop = true;
        else {
            s16 left = (s16)rd16(c + CH_LEFT);
            if (left < 0) stop = false;                 /* negative: loops for ever */
            else {
                left--;
                wr16(c + CH_LEFT, (u16)left);
                stop = left < 0;                        /* stops when left was 0 */
            }
        }
        if (stop) {
            wr32(c + CH_TARGET, 0xFFFFFFFFu);
            paula_set_vol(ch, 0);
            wr32(c + CH_VOL, 0);
            paula_set_per(ch, 0x96);
            paula_dmacon((u16)(1u << ch));
            wr32(c + CH_STOP, D32(D_sfx_tick));
            wr32(c + CH_DATA, 0);
        }
        paula_intreq(bit);
    }
}

/* 0x126AE: Sfx VBLInt, priority 0x1E. */
static void sfx_vbl_server(void)
{
    SETD32(D_sfx_tick, D32(D_sfx_tick) + 1);
    SETD16(D_sfx_saved_intena, paula_intenar() & 0x0780);
    paula_intena(0x0780);                               /* keep the audio interrupt out while editing records */
    SETD16(D_sfx_dma_set, 0x8000);
    for (u16 ch = 0; ch < 4; ch++) {
        APTR c = chan(ch);
        if (rd16(c + CH_PENDING) != 0 && (s32)(D32(D_sfx_tick) - rd32(c + CH_STOP)) >= 2) {
            paula_set_lc(ch, rd32(c + CH_DATA));
            paula_set_len(ch, rd16(c + CH_LEN));
            paula_set_per(ch, rd16(c + CH_PERIOD));
            paula_set_vol(ch, rd16(c + CH_VOL));
            wr16(c + CH_LEFT, rd16(c + CH_REPEATS));
            SETD16(D_sfx_dma_set, D16(D_sfx_dma_set) | (1u << ch));
            wr16(c + CH_PENDING, 0);
        }
        if ((s16)rd16(c + CH_TARGET) >= 0) {            /* tst.w on the high word */
            s32 v = (s32)rd32(c + CH_VOL), target = (s32)rd32(c + CH_TARGET), step = (s32)rd32(c + CH_STEP);
            bool done;
            if (v == target) done = true;
            else if (v > target) { v = (s32)((u32)v - (u32)step); done = v <= target; }
            else                 { v = (s32)((u32)v + (u32)step); done = v >= target; }
            if (done) { v = target; wr32(c + CH_TARGET, 0xFFFFFFFFu); }
            wr32(c + CH_VOL, (u32)v);
            paula_set_vol(ch, (u16)((u32)v >> 16));     /* written even when the channel is idle */
        }
    }
    u16 saved = D16(D_sfx_saved_intena);
    if (saved) paula_intena((u16)(saved | 0x8000));
    if (D8(D_sfx_dma_set + 1)) paula_dmacon(D16(D_sfx_dma_set));   /* tst.b on the low byte */
}

/* ---------------------------------------------------------------- song player */

void song_init(void)                                    /* 0x1278C */
{
    if (!D16(D_song_installed)) {
        SETD32(D_song_server_a4, A4_VALUE);
        SETD16(D_song_state, 0);
        SETD32(D_song_any_active, 0);                   /* clr.l: also the high word of D:0736 */
        SETD8(D_song_vbl_interrupt + IS_TYPE, 2);
        SETD8(D_song_vbl_interrupt + IS_PRI, 0x20);
        SETD32(D_song_vbl_interrupt + IS_NAME, ADDR_SONG_VBL_NAME);
        SETD32(D_song_vbl_interrupt + IS_DATA, D32(D_song_server_a4));
        SETD32(D_song_vbl_interrupt + IS_CODE, ADDR_SONG_VBL);
        ahost_add_vbl_server(0x20, song_vbl_server);
    }
    SETD16(D_song_installed, 1);
    song_set_volume(0x10, 0);
}

static void song_free_instruments(void);

void song_shutdown(void)                                /* 0x127F4 */
{
    if (D16(D_song_installed)) {
        song_request_stop();
        ahost_delay(3);
        song_free_instruments();
        ahost_remove_vbl_server(song_vbl_server);
    }
    SETD16(D_song_installed, 0);
}

/* 0x12826: names = char *[4] in amem (a value <= 10 = "the sample of slot n"), oneshot = int[4]. */
static void song_load_instruments(APTR names, APTR oneshot)
{
    if (!D16(D_song_inst_loaded)) {
        for (int i = 0; i < 4; i++) {
            u32 name = rd32(names + 4u * (u32)i);
            if (name <= 10) {                           /* cmpi.l #10 ; bls */
                SETD32(D_song_inst_ptr + 4 * i, D32(D_song_inst_ptr + 4 * name));
                SETD16(D_song_inst_owned + 2 * i, 0);
            } else {
                char s[64], path[80];
                amem_cstr(name, s, sizeof s);
                snprintf(path, sizeof path, "songs/%s", s);
                SETD32(D_song_inst_ptr + 4 * i, load_file_chip(path));   /* 0 on failure */
                SETD16(D_song_inst_owned + 2 * i, 1);
            }
            SETD16(D_song_inst_oneshot + 2 * i, rd16(oneshot + 2u * (u32)i));
        }
    }
    SETD16(D_song_inst_loaded, 1);
}

static void song_free_instruments(void)                 /* 0x128FE */
{
    if (D16(D_song_inst_loaded))
        for (int i = 0; i < 4; i++)
            if (D16(D_song_inst_owned + 2 * i)) free_mem(D32(D_song_inst_ptr + 4 * i));
    SETD16(D_song_inst_loaded, 0);
}

void song_set_volume(s16 vol, s32 step)                 /* 0x12942 */
{
    SETD32(D_song_vol_step, (u32)step);
    SETD32(D_song_vol_target, (u32)(s32)vol << 16);
}

s16 song_get_volume(void)                               /* 0x1295E */
{
    return D16(D_song_state) ? (s16)D16(D_song_volume) : 0;
}

s16 song_is_active(void)                                /* 0x12A40 */
{
    u16 s = D16(D_song_state);
    return (s != 0 && s != 3) ? -1 : 0;
}

static void song_request_stop(void)                     /* 0x1299C */
{
    if (D16(D_song_state)) SETD16(D_song_state, 3);
    while (song_is_active()) ahost_pump();              /* returns at once: the state is now 0 or 3 */
}

/* 0x129BA: a0 song, a1 inst[4], a2 oneshot[4] (both in amem), d0 base ticks. */
static void song_setup(APTR smus, APTR inst, APTR oneshot, u16 base)
{
    if (!D16(D_music_enabled)) return;
    SETD32(D_song_data, smus);
    for (int k = 0; k < 4; k++) {
        wr32(track(k) + TR_INST, rd32(inst + 4u * (u32)k));
        wr16(track(k) + TR_ONESHOT, rd16(oneshot + 2u * (u32)k));
    }
    u16 d = base;
    for (int i = 4; i >= 0; i--) { SETD16(D_note_ticks + 2 * i, d); d = (u16)(d << 1); }       /* 96..6 */
    d = (u16)(base + (base >> 1));
    for (int i = 12; i >= 8; i--) { SETD16(D_note_ticks + 2 * i, d); d = (u16)(d << 1); }      /* 144..9 */
    SETD16(D_song_state, 1);
}

static void song_start(APTR smus)                       /* 0x12972 */
{
    if (D16(D_song_installed) && D16(D_music_enabled)) {
        song_request_stop();
        song_setup(smus, DADDR(D_song_inst_ptr), DADDR(D_song_inst_oneshot), 6);
    }
}

static void song_volume_slide(void)                     /* 0x12C98 */
{
    if ((s16)D16(D_song_vol_target) < 0) return;        /* tst.w on the high word */
    s32 v = DS32(D_song_volume), target = DS32(D_song_vol_target), step = DS32(D_song_vol_step);
    if (step != 0 && v != target) {
        if (v < target) {
            v = (s32)((u32)v + (u32)step);
            if (v < target) { SETD32(D_song_volume, (u32)v); return; }
        } else {
            v = (s32)((u32)v - (u32)step);
            if (v > target) { SETD32(D_song_volume, (u32)v); return; }
        }
    }
    SETD32(D_song_volume, (u32)target);
    SETD32(D_song_vol_target, 0xFFFFFFFFu);
}

/* 0x12CDC: searches for the long `id` from *off in 2-byte steps while *off < limit (word compare). */
static bool smus_find_long(APTR base, u16 *off, u32 limit, u32 id)
{
    do {
        if (rd32(base + (u32)(s32)(s16)*off) == id) return true;
        *off = (u16)(*off + 2);
    } while ((s16)*off < (s16)(u16)limit);
    return false;
}

/* 0x12A54: Song VBLInt, priority 0x20. PORT: the songs were timed on a PAL machine (50 VBL a second, quarter
 * note = 24 ticks = 0.48 s), so on the port's 60 Hz VBL the server skips every sixth tick to keep their tempo. */
static void song_vbl_server(void)
{
    static u32 phase;
    phase += SONG_HZ;
    if (phase < VBL_HZ) return;
    phase -= VBL_HZ;

    if (D16(D_song_state) == 0) return;
    song_volume_slide();

    if (D16(D_song_state) == 1) {                       /* (re)start: parse the file */
        /* ADKCON = 0x00FF: nothing to emulate */
        for (int k = 0; k < 4; k++) {
            wr16(track(k) + TR_ACTIVE, 0);
            if (!g_original_bugs) wr16(track(k) + TR_TIE, 0);   /* bug 12 fix: no tie carried into a song */
        }
        APTR p = D32(D_song_data);
        u32 limit = rd32(p - 0x10);                     /* allocation size in the block header */
        u16 off = 0;
        if (smus_find_long(p, &off, limit, 0x53484452u /* 'SHDR' */)) {
            APTR at = p + (u32)(s32)(s16)off;
            SETD16(D_smus_tempo, rd16(at + 8));                         /* unused */
            SETD16(D_smus_volume, (u16)(s16)(s8)rd8(at + 10));          /* unused */
            for (int k = 0; k < 4; k++) {
                off = (u16)(off + 2);
                if (!smus_find_long(p, &off, limit, 0x5452414Bu /* 'TRAK' */)) break;
                at = p + (u32)(s32)(s16)off;
                APTR t = track(k);
                wr16(t + TR_ACTIVE, 0xFFFF);
                wr32(t + TR_SIZE, rd32(at + 4));
                wr32(t + TR_DATA, at + 8);
                wr32(t + TR_POS, 0);
                wr16(t + TR_COUNT, 0);                  /* the original leaves the tie flag (+16) alone */
            }
        }
        SETD16(D_song_state, 2);
        return;
    }
    if (D16(D_song_state) == 3) {
        snd_stop_all();
        SETD16(D_song_state, 0);
        return;
    }

    /* state 2: one tick */
    SETD16(D_song_any_active, 0);
    SETD16(D_song_dma_on, 0);
    SETD16(D_song_dma_off, 0);
    for (int k = 0; k < 4; k++) {                       /* d5 = 3..0, channel k = 3 - d5 */
        APTR t = track(k);
        u16 bit = (u16)(1u << k);                       /* 8 >> d5 */
        if (!rd16(t + TR_ACTIVE)) break;                /* later tracks are skipped */
        SETD16(D_song_any_active, 0xFFFF);
        u16 count = rd16(t + TR_COUNT);
        if (count != 0) {
            count--;
            wr16(t + TR_COUNT, count);
            if (count == 1 && !rd16(t + TR_TIE)) paula_set_vol(k, 0);  /* cut one tick early */
            continue;
        }
        APTR data = rd32(t + TR_DATA);
        u16 id, arg;
        for (;;) {
            s32 pos = (s32)rd32(t + TR_POS);
            if (pos >= (s32)rd32(t + TR_SIZE)) break;
            APTR e = data + (u32)(s32)(s16)(u16)pos;    /* (a0, d0.w) */
            id = rd8(e);
            arg = rd8(e + 1);
            wr32(t + TR_POS, (u32)(pos + 2));
            if (id <= 0x80) goto event;                 /* 0x81..0xFF: skipped, take no time */
        }
        paula_set_vol(k, 0);                            /* end of track */
        SETD16(D_song_dma_off, D16(D_song_dma_off) | bit);
        wr16(t + TR_ACTIVE, 0);
        continue;
    event:
        wr16(t + TR_COUNT, (u16)(D16(D_note_ticks + 2 * (arg & 0x0F)) - 1));   /* chord and tuplet bits ignored */
        if (id == 0x80) {                               /* rest; DMA keeps running */
            paula_set_vol(k, 0);
            wr16(t + TR_TIE, 0);
        } else if (rd16(t + TR_ONESHOT)) {
            play_sample(rd32(t + TR_INST), (s16)k, (s16)D16(D_song_volume));  /* note ignored; tie untouched */
        } else {
            if (!rd16(t + TR_TIE)) {                    /* a tied note does not retrigger */
                APTR s = rd32(t + TR_INST);
                paula_set_vol(k, 0);
                u16 len = rd16(s + 2);                  /* low word of the length long */
                /* Bug 10: the original divides by the length (17 for BuzzSynth) while Paula plays len >> 1
                 * words = 16 bytes, so every waveform note is 17/16 sharp; fixed: divide by the bytes played. */
                u16 div = g_original_bugs ? len : (u16)((len >> 1) << 1);
                u32 table = D32(D_note_period_table + 4 * id);   /* D:05F0 + 4 * (id - 0x3C) */
                s16 per = (s16)divu_w(table, div);
                if (per < 0x7C) per = 0x7C;             /* signed compare */
                paula_set_per(k, (u16)per);
                paula_set_lc(k, s + 6);
                paula_set_len(k, (u16)(len >> 1));
                paula_set_vol(k, D16(D_song_volume));
                SETD16(D_song_dma_on, D16(D_song_dma_on) | bit);
            }
            wr16(t + TR_TIE, (arg & 0x40) ? 0xFFFF : 0);
        }
    }
    u16 on = (u16)(D16(D_song_dma_on) & ~paula_dmaconr() & 0xF);
    if (on) paula_dmacon((u16)(on | 0x8000));           /* only channels now off */
    u16 off = (u16)(D16(D_song_dma_off) & paula_dmaconr() & 0xF);
    if (off) paula_dmacon(off);
    SETD16(D_song_state, D16(D_song_any_active) ? 2 : 1);   /* track 0 finished: restart next tick */
}

/* 0x12CF4: file = host string (a literal in the code hunk), inst = char *[4], oneshot = int[4] in amem. */
static void song_play(const char *file, APTR inst, APTR oneshot, u16 mask, s16 vol)
{
    if (D32(D_song_buffer)) {
        char name[64];
        amem_cstr(DADDR(D_song_name), name, sizeof name);
        if (strcmp(name, file) != 0) song_stop();
    }
    if (D32(D_song_buffer) == 0 && D16(D_music_enabled)) {
        snd_stop_all();
        char path[80];
        snprintf(path, sizeof path, "songs/%s", file);
        size_t n = strlen(file);
        amem_put(DADDR(D_song_name), file, (u32)n + 1);  /* sprintf(song_name, "%s", file) */
        SETD32(D_song_buffer, load_file(path));
        song_load_instruments(inst, oneshot);
        sfx_set_song_channels(mask);
        song_set_volume(vol, 0);                        /* immediate */
        if (D32(D_song_buffer)) song_start(D32(D_song_buffer));
    } else {
        song_set_volume(vol, 0x8000);                   /* same song (or music off): fade to vol */
    }
}

void song_stop(void)                                    /* 0x12DB0 */
{
    if (!D32(D_song_buffer)) return;
    song_set_volume(0, 0x5400);
    while (song_get_volume() != 0) ahost_pump();        /* busy wait on the VBL server */
    song_request_stop();
    ahost_delay(2);                                     /* the server: state 3 -> snd_stop_all */
    song_free_instruments();
    free_mem(D32(D_song_buffer));
    SETD32(D_song_buffer, 0);
    sfx_set_song_channels(0);
}

void song_play_testdrive(void)  { song_play("TestDrive.Iff.Sng",  DADDR(D_testdrive_inst), DADDR(D_testdrive_oneshot), 1, 0x20); }
void song_play_test2(void)      { song_play("Test2.Iff.Sng",      DADDR(D_song_inst), DADDR(D_song_oneshot), 3, 0x16); }
void song_play_testgas(void)    { song_play("TestGas.Iff.Sng",    DADDR(D_song_inst), DADDR(D_song_oneshot), 3, 0x16); }
void song_play_endsuccess(void) { song_play("EndSuccess.Iff.Sng", DADDR(D_song_inst), DADDR(D_song_oneshot), 3, 0x16); }
void song_play_loser(void)      { song_play("Loser.Iff.Sng",      DADDR(D_song_inst), DADDR(D_song_oneshot), 3, 0x16); }
