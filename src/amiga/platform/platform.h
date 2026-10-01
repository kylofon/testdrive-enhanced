#pragma once
/* The original's root-hunk platform layer, ported (port/amiga/spec/platform_video.md): display, pictures,
 * files and memory, the Ticks VBL server, keys and the joystick. Pointers into the 68000 memory are APTRs. */
#include "../amem.h"

/* ---- display (video.c) */
void display_init(void);                         /* 0x115BC */
void display_shutdown(void);                     /* 0x11756 */
void show_view(APTR view);                       /* 0x1C62E (overlay 1, used everywhere) */
void ilbm_to_view(APTR iff, APTR view);          /* 0x1063A */
void view_clear(APTR view);                      /* 0x1574E */
void view_copy(APTR src, APTR dst);              /* 0x1580C */
void view_copy_palette(APTR src, APTR dst);      /* 0x1582C */
void view_copy_bitmaps(APTR src, APTR dst);      /* 0x1587C */
void view_dissolve(APTR src_or_0, APTR dst);     /* 0x109D0 */
void iff_next_chunk(APTR base, s32 *off);        /* 0x10692 */
void cmap_to_rgb4(APTR chunk, u16 *pal);         /* 0x107B4 */
void unpack_byterun1_row(APTR *src, APTR *dst, s16 n);   /* 0x15344 */

/* ---- files and memory (files.c) */
APTR mem_alloc(u32 size, u32 flags);             /* 0x15930 */
APTR alloc_public(u32 size);                     /* 0x15904 */
APTR alloc_chip(u32 size);                       /* 0x1591A */
s16  free_mem(APTR p);                           /* 0x159FA; p = 0xFFFFFFFF frees everything */
APTR load_file(const char *name);                /* 0x149D2 */
APTR load_file_chip(const char *name);           /* 0x149E8 */
APTR load_file_flags(const char *name, u32 flags); /* 0x149FE: result also in D:24BC, size in D:2810 */

/* ---- timing and input (input.c) */
void vbl_install(void);                          /* 0x1180E */
void vbl_remove(void);                           /* 0x1185C */
void input_init(u16 passmask);                   /* 0x154E4 */
void input_shutdown(void);                       /* 0x155C4 */
s16  key_available(void);                        /* 0x156EC: -1 when a key is queued */
u32  get_key(void);                              /* 0x156F8: qualifier << 16 | raw code; waits */
s16  get_char(void);                             /* 0x15628 */
s16  key_to_ascii(u32 key);                      /* 0x1563A */
void fire_port_init(void);                       /* 0x153CC */
s16  read_fire(void);                            /* 0x15402 / 0x11872: 1 = pressed */
s16  read_joy(void);                             /* 0x15416 / 0x11886: 0 centre, 1 up .. 8 up-left, clockwise */
s16  joy_fire(void);                             /* 0x153E4 */
s16  joy_dir(void);                              /* 0x153EC */

/* ---- system (system.c) */
s16  rand16(void);                                /* 0x1530E */
s16  quit_requested(void);                        /* 0x1544E: sticky Ctrl-C (D:1BDA) */
void poll_input(void);                           /* 0x10462: P, M, S, D, O hot keys, Ctrl-R -> D:0346 */
void dos_Delay(u32 fiftieths);                   /* 0x175EA */

/* The O-mode gear selection inside the VBL server calls drive_sim's gear_select 0x1EE76 and 0x20826; the drive
 * installs them here (0 outside the drive, where the original would have crashed, platform_video §4.8). */
extern void (*vbl_gate_select)(u16 gear);
