#pragma once
/* Paula: the four DMA audio channels and their interrupts (port/amiga/spec/platform_audio.md §5).
 *
 * Registers are written as the game writes them (AUDxLC/LEN/PER/VOL, DMACON, INTENA, INTREQ). Each channel
 * plays 8-bit signed samples from amem[] at `clock / period` bytes per second, zero-order hold, and reloads
 * LC/LEN from the current registers after every pass, raising its audio interrupt (INTREQ bit 7 + ch) at DMA
 * start and at every reload. The level-4 handler runs at that exact sample, inside the tick's rendering.
 * Channels 0 and 3 go left, 1 and 2 right. Clock: NTSC, 3579545 Hz (the game's constant 0x369E99). */
#include "amem.h"

#define PAULA_CLOCK 3579545u

void paula_init(void);                 /* installs Paula as the host's audio source */

void paula_set_lc(int ch, APTR lc);
void paula_set_len(int ch, u16 words); /* 0 = 65536 words */
void paula_set_per(int ch, u16 period);
void paula_set_vol(int ch, u16 vol);   /* 0..64; more acts as 64 */

/* DMACON/INTENA/INTREQ: bit 15 set = set the other bits, clear = clear them. Only the audio bits
 * (DMACON 0-3, INTENA/INTREQ 7-10) have an effect; DMAEN and INTEN are taken as on. */
void paula_dmacon(u16 v);
u16  paula_dmaconr(void);
void paula_intena(u16 v);
u16  paula_intenar(void);
void paula_intreq(u16 v);
u16  paula_intreqr(void);

/* The level-4 autovector ($70): called when an enabled audio interrupt is requested. */
void paula_set_level4_handler(void (*handler)(void));

/* The A500's LED low-pass filter (CIA-A PRA bit 1; the game never changes it, so it is on). The fixed RC
 * low-pass of the A500 output stage is always applied. */
void paula_set_led_filter(bool on);
