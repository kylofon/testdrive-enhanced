#pragma once
/* The game side of the Amiga port: program flow, title sequence, car selection (port/amiga/spec/game_flow.md,
 * title_select.md). Pointers into the 68000 memory are APTRs; int is 16 bits (s16) as in Aztec C. */
#include <stddef.h>

#include "../amem.h"

/* ---- title and car selection, overlay 1 (title.c; title_select.md) */
s16  run_intro(void);                                   /* 0x1AED0: -1 ran to the end, 1 interrupted */
void intro_accolade_bull(APTR titleCar);                /* 0x1B2B2 */
void intro_testdrive_car(APTR titleCar);                /* 0x1B396 */
void intro_testdrive_logo(APTR titleCar);               /* 0x1B62E */
s16  car_select(void);                                  /* 0x1B8E6: car index, or -1 */
void showroom_display_init(void);                       /* 0x1BA7C */
void showroom_display_free(void);                       /* 0x1BC64 */
void car_pics_free_all(void);                           /* 0x1BD14 */
APTR load_file_retry(const char *name);                 /* 0x1BD9C */
void show_car(s16 idx, APTR view, s16 dir);             /* 0x1BDCA: dir 0, 1 (down, next) or -1 (up, previous) */
void sb_load_ilbm(APTR data, APTR view);                /* 0x1BF40 */
void sb_parse_ilbm(APTR form, APTR view);               /* 0x1BF8E */
void sb_decode_body(APTR bmhd, const u16 *colors, APTR body, APTR view);   /* 0x1C068 */
void colormap_clear(APTR cm);                           /* 0x1C15C */
void fade_plate_colours(void);                          /* 0x1C190 */
void wait_frames(s16 n);                                /* 0x1C22A */
void showroom_drive_away(s16 idx);                      /* 0x1C244 */
void scroll_out_down(APTR view);                        /* 0x1C682 */
void scroll_in_up(APTR view);                           /* 0x1C700 */
void scroll_out_up(APTR view);                          /* 0x1C76A */
void scroll_in_down(APTR view);                         /* 0x1C7CC */
void start_scroller(s16 up, APTR view);                 /* 0x1C83C (TDScroller, run synchronously) */
void res_find_list(APTR archive, APTR names, APTR out, APTR pool);   /* 0x10B22 (root) */

/* ---- Cars.txt (cars.c; game_flow.md) */
void load_cars_txt(s16 load);                           /* 0x10346 */
APTR skip_line(APTR *pp);                               /* 0x10402 */
/* Helpers of the port: the C runtime's sscanf("%d ...") on a string in amem[] (returns the number of values
 * converted; the others are left alone), and a copy of a string in amem[] into a host buffer. */
int  amem_scan_ints(APTR s, s16 *vals, int n);
void amem_cstr(APTR s, char *dst, size_t n);

/* ---- program flow (main.c) */
s16  high_scores(s32 score, s16 car);                   /* 0x12F28: 0, or -1 on a timeout */

/* ---- the drive's flow, overlay 2 (drive.c; game_flow.md §4) */
s16  run_game(s16 car);                                 /* 0x1C900: 0 game over, 1 Ctrl-C */
s16  play_again_menu(void);                             /* 0x1D5C8: 1 same car, 0 new car */

/* ---- STUBS: parts of the flow ported by later stages (stubs.c). Each replaces one original function. */
s16  scores_load(APTR names, APTR scores, APTR cars);   /* 0x13028: 0 ok, 1 = table reset (rewrite) */
void scores_save(APTR names, APTR scores, APTR cars);   /* 0x1313E */
void scores_enter_name(s32 score, s16 car, APTR names, APTR scores, APTR cars);   /* 0x13708 */
s16  scores_show(APTR names, APTR scores, APTR cars);   /* 0x13BD8: 0 fire / Ctrl-C, -1 timeout */
s16  credits_show(void);                                /* 0x13A9E: 0 fire / Ctrl-C, -1 timeout */
void stage_results(s16 car, s32 avgSpeed, s16 secs, s32 score, s16 limit);   /* 0x147E4 (gas station) */
void dealership_ending(void);                           /* 0x1CF76 */
