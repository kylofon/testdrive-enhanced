#pragma once
/* Enhanced renderer (see enhanced.c). The hooks are called from run_stage (game/flow_stage.c). */
#include "../mem.h"

void enh_init(void);              /* installs the screen overlay; call before the game starts */
extern bool enh_sprite_detail_max; /* --sprite-detail: true = max (the largest sprite scale everywhere) */

void enh_stage_begin(void);       /* after the stage buffers are set up */
void enh_stage_end(void);         /* overlay off (stage left, or a full-screen picture follows) */
void enh_life_reset(void);        /* after reset_car_state(): snap smoothed values */
void enh_frame(void);             /* after present_road_buffer(): render the road window */
void enh_gear_box(void);          /* draw_gear_box() with its frame-counted close delay kept at 8 Hz */
void enh_crash_sequence(void);    /* replaces crash_windscreen_sequence() */
void enh_cover_sprite(FarPtr mask); /* keep the EGA image where this AND mask (own position) is opaque */
