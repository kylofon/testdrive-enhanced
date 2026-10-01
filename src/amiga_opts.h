#pragma once
/* The Amiga features (--amiga): parts of the Amiga release of Test Drive that replace the DOS game's, each on
 * its own so they can be mixed. The Amiga ones need the Amiga disk (--amiga-disk: the .adf, a folder holding
 * it, or a folder with the files of the disk); the DOS game files are always needed too. */
#include "types.h"

typedef struct {
    bool intro;        /* the title sequence */
    bool menu;         /* car selection, showroom and the play-again menu */
    bool dashboards;   /* the cockpits: dashboard, gauges, wheel, gear knob, roof, post, hood, mirror */
    bool music;        /* the songs, played on the emulated Paula */
    bool projection;   /* the road's shape: the Amiga's walk of the road records */
    bool sky;          /* the blue sky with drifting clouds */
    bool cliffs;       /* the brown rock face */
    bool traffic;      /* the traffic's speeds */
    bool cops;         /* the police: radar zones, speed, chase and ticket */
    bool wide_roads;   /* the passing lanes of stages 1 and 2 (the road records' width codes) */
    bool signs;        /* the road signs and poles, and the speed limits 30 / 55 / 65 */
} AmigaOpts;

extern AmigaOpts amiga;
extern const char *amiga_disk;     /* --amiga-disk; default "Game Amiga" */

/* Parses the --amiga list: "all" or names separated by commas (intro, menu, dashboards, music, projection,
 * sky, cliffs, traffic, cops, wide-roads, signs). False if a name is unknown. */
bool amiga_parse(const char *list);
/* True if any feature needs the Amiga disk. */
bool amiga_any(void);
