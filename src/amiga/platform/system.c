/* System pieces — port of td 0x1530E (rand16), 0x1544E (quit_requested), 0x10462 (poll_input) and the dos
 * Delay glue 0x175EA (port/amiga/spec/platform_video.md §4.10, §4.11). */
#include "platform.h"

#include "audio.h"
#include "../ahost.h"
#include "../asymbols.h"

/* VHPOSR ($DFF006): the beam position, (line & 0xFF) << 8 | horizontal colour clock. The port derives it from
 * the host clock's position inside the 1/60 s frame (NTSC: 262 lines of 227 clocks), which is as
 * unpredictable as the real beam (the spec only needs a non-reproducible value). */
static u16 vhposr(void)
{
    const u64 frame_ns = 1000000000u / VBL_HZ;
    u64 pos = (ahost_time_ns() % frame_ns) * (262u * 227u) / frame_ns;
    u32 v = (u32)(pos / 227u), h = (u32)(pos % 227u);
    return (u16)((v & 0xFF) << 8 | h);
}

s16 rand16(void)                                         /* 0x1530E */
{
    if (D16(D_rand_seeded) == 0) {
        SETD16(D_rand_seed, vhposr());
        SETD16(D_rand_seeded, 0xFFFF);
    }
    s32 v = (s32)(s16)D16(D_rand_seed) * 0x1AFB + 0x1FCCD;   /* muls.w; add.l; the low word is kept */
    SETD16(D_rand_seed, (u16)v);
    SETD16(D_rand_seed, D16(D_rand_seed) ^ vhposr());
    return (s16)D16(D_rand_seed);
}

/* Aztec Chk_Abort 0x17418: SetSignal(0, SIGBREAKF_CTRL_C). With the shipped `p` argument every key-down is
 * queued and swallowed by the input handler, so no Ctrl-C ever reaches the task: always 0 in the port (closing
 * the window quits through the host instead). */
static s32 chk_abort(void) { return 0; }

s16 quit_requested(void)                                 /* 0x1544E */
{
    SETD16(D_Enable_Abort, 0);                           /* keeps Chk_Abort from exiting */
    if (D16(D_quit_flag) == 0 && chk_abort() != 0) SETD16(D_quit_flag, 0xFFFF);
    return (s16)D16(D_quit_flag);
}

static u8 to_upper(u8 c) { return c > 0x60 && c <= 0x7A ? (u8)(c - 0x20) : c; }   /* 0x15D1C */

/* 0x10462: one queued key. P pauses (drive only; the resume key is thrown away), M music, S sound, D gearbox
 * display and O gate shifting (drive only), Ctrl-R (converted key 0x12) sets D:0346. */
void poll_input(void)
{
    if (!key_available()) return;
    u8 c = (u8)key_to_ascii(get_key());
    c = to_upper(c);
    if (c == 'P' && D16(D_g_inGame)) {
        u16 old = D16(D_drawer_pause);
        SETD16(D_drawer_pause, 1);
        while (!key_available()) dos_Delay(1);
        get_key();
        SETD16(D_drawer_pause, old);
    } else if (c == 'M') {
        SETD16(D_music_enabled, D16(D_music_enabled) ? 0 : 1);
        if (!D16(D_music_enabled)) song_stop();
    } else if (c == 'S') {
        SETD16(D_sfx_enabled, D16(D_sfx_enabled) ? 0 : 1);
        if (!D16(D_sfx_enabled)) snd_stop_all();
    } else if (c == 'D' && D16(D_g_inGame)) {
        SETD16(D_gearbox_always, D16(D_gearbox_always) ? 0 : 1);
        u8 v = (u8)((s16)D16(D_gearbox_always) * 0xFF); /* muls.w #$FF; the low byte to both */
        SETD8(D_gearbox_show + 1, v);
        SETD8(D_gearbox_show, v);
    } else if (c == 'O' && D16(D_g_inGame)) {
        SETD16(D_gate_mode, D16(D_gate_mode) ? 0 : 1);
    } else if (c == 0x12) {
        SETD16(D_g_abortKey, 1);
    }
}

void dos_Delay(u32 fiftieths) { ahost_delay(fiftieths); }   /* 0x175EA: dos.library Delay, 1/50 s */
