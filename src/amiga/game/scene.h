#pragma once
/* The drive's picture — port of overlay 2's renderer (port/amiga/spec/drive_scene.md): the road walker and the
 * mirror, the object queue, cockpit and gauges, clouds, bugs and the crash.
 *
 * The original renders in a second exec task ("Road Drawer") that draws frames as fast as the CPU allows while
 * the main task runs one logic frame per 5 vertical blanks. The port is single-threaded: the drive flow calls
 * road_drawer_step() once per 60 Hz tick (it renders one frame from the snapshot and flips), and the
 * simulation runs every 5th tick. */
#include "../amem.h"

void render_frame(APTR back_view);       /* 0x1D392: snapshot, road window, gauges */
void update_gauges(void);                /* 0x20AF4: needles or the digital cluster */
void redraw_cockpit_back(void);          /* 0x1FDF4: the cockpit shapes into the back view */
void road_drawer_start(void);            /* 0x24E94: what the original needs to start the task */
void road_drawer_stop(void);             /* what the original's RemTask path does */
void road_drawer_step(void);             /* 0x1D484 body, one frame: render, text, show_view */
void crash_sequence(void);               /* 0x1FBD8 */
void init_clouds(void);                  /* 0x1EB78 */
void reset_wheel_state(s16 keep);        /* 0x1ED8A */
void row_road_widths(void);              /* 0x1E5C6: row 0 widths the simulation reads (D:0D94/D:0D96) */
void radar_lamp(void);                   /* 0x1EC94 */
void wheel_marker(void);                 /* 0x247B2 */
void dash_views_setup(void);             /* 0x24D8A / 0x1F5BA area: dashboard views, palette split, sprites */

/* The rest of the renderer the stage setup and shutdown (game_flow's 0x24ACA / 0x24CE4) need. */
void setup_drive_copper(APTR view);      /* 0x24D8A: sprite pointers + palette split for one View */
void remake_view(APTR view);             /* 0x24D68: MakeVPort + MrgCop */
void fade_out_drive(void);               /* 0x24E52 */
void fade_out_palette(s16 delay, APTR pal2, s16 line);   /* 0x1FEAE (the gas station uses it too) */
void sprites_init(void);                 /* 0x1F5BA: sprite data, needle/knob/dot segments, sprite DMA */
void sprites_free(void);                 /* 0x1F99A */
void load_cockpit_shapes(void);          /* 0x24FAA: `<car>Dash.Shp` lookups and masks */
void load_road_shapes(void);             /* 0x253E6: 0x24FAA plus everything in `Pics/Road.Shp` */

/* Sprite segments and the drive's user copper lists; the ending (game/screens.c) builds the same list. */
void sprite_set_segment(u16 word, s16 h, s16 v0, s16 v1, s16 att, int n);   /* 0x1F05A */
void ucop_sprite_pointers(APTR ucl);     /* 0x1FAD4: CWait(-10), the eight SPRxPT/POS/CTL moves */
void ucop_palette_split(APTR ucl, APTR pal, s16 line);   /* 0x1FB4A: 32 colours from `line` */
