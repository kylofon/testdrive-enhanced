#pragma once
/* game_flow private declarations (image 0x0000-0x1F4D). Shared prototypes live in game.h. */
#include <stdio.h>
#include "game.h"

/* DGROUP string / buffer at DS:o (the original passes these near pointers directly). */
#define DSTR(o) ((char *)mp(DGROUP, (u16)(o)))

/* DS:7F1A offscreen page descriptor, and the sprite it describes ("*g_offscreenPage": hdr_off:plane_seg0). */
static inline FarPtr flow_page_desc(void)   { return far_rd(DGROUP, DS_page_buf_desc); }
static inline FarPtr flow_page_sprite(void) { FarPtr d = flow_page_desc(); return far_rd(d.seg, d.off); }
/* g_carNames[idx] (near char* table DS:78F2 into DS:7F64). */
static inline char  *flow_car_name(int idx) { return DSTR(DSW(DS_g_carNames + (u16)(idx * 2))); }

/* ---- MSC text-mode stdio emulation over a whole file read in binary mode (CR LF -> LF, ^Z = EOF). */
typedef struct { unsigned char *data; size_t len, pos; } FlowText;
bool  flow_text_open(FlowText *t, const char *name);
char *flow_text_gets(char *s, int n, FlowText *t);                 /* fgets */
int   flow_text_scan_2d(FlowText *t, int *a, int *b);              /* fscanf(f, "%d %d\n", a, b) */
void  flow_text_close(FlowText *t);
FILE *flow_fopen_game(const char *name, bool create, const char *mode);

/* 0x94C7 res_find_list semantics with C-side output (the original's outputs are stack arrays). */
int   flow_find_list(FarPtr arc, char *names, FarPtr *out, int max);

/* ---- flow.c */
void  load_cars_txt(void);                                          /* 0x0243 */
int   play_again_menu(void);                                        /* 0x02BF */
int   run_intro(void);                                              /* 0x037A */
int   intro_accolade(void);                                         /* 0x03F4 */
int   intro_testdrive_car(void);                                    /* 0x051B */
int   intro_testdrive_logo(void);                                   /* 0x0792 */

/* ---- flow_screens.c */
int   car_select(void);                                             /* 0x098C */
void  show_car(int idx, int dir);                                   /* 0x0AB2 */
void  showroom_drive_away(int idx);                                 /* 0x0D75 */
void  read_line_strip(char *buf, int n, FlowText *f);               /* 0x1000 */
int   credits_show(void);                                           /* 0x186E */

/* ---- flow_amiga.c (AMIGA: the --amiga features of the flow) */
void  flow_amiga_start(void);                                       /* opens the Amiga disk if a feature needs it */
int   flow_amiga_intro(void);                                       /* as run_intro */
int   flow_amiga_car_select(void);                                  /* as car_select */
int   flow_amiga_play_again(void);                                  /* as play_again_menu (no Esc) */
void  flow_song_play(u16 ds_song);                                  /* snd_play_oneshot of DS:ds_song, or Amiga's */
void  flow_song_stop(void);                                         /* snd_stop_oneshot, and the Amiga song */
void  flow_ending_song(bool on);                                    /* Amiga music: EndSuccess at the dealership */

/* ---- flow_scores.c */
int   high_scores(s32 new_score);                                   /* 0x12DB */
void  scores_load(char *names, s32 *scores, char *cars);            /* 0x1369 */
void  scores_enter_name(s32 score, int car, char *names, s32 *scores, char *cars); /* 0x1482 */
void  scores_save(char *names, s32 *scores, char *cars);            /* 0x162E */
int   scores_show(char *names, s32 *scores, char *cars);            /* 0x16D7 */

/* ---- flow_stage.c (run_game / run_stage are in game.h) */
s32   stage_score(void);                                            /* 0x125D */
void  stage_results(s32 score);                                     /* 0x1993 */
void  results_text(s32 score);                                      /* 0x1B36 */
void  results_add_line(const char *s);                              /* 0x1CFA */
void  results_scroll(void);                                         /* 0x1D20 */
