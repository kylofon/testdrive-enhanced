#pragma once
/* The drive's simulation — port of overlay 2's state updates (port/amiga/spec/drive_sim.md). One logic frame
 * per drive frame (5 vertical blanks = 12 Hz on the NTSC clock the port runs, README Decisions 1). The
 * renderer (game/scene.h) draws from these globals; the drive flow (game/drive.h) calls them. */
#include "../amem.h"

void sim_controls(void);                 /* 0x20CB0: input, physics, position — the per-frame entry */
void engine_update(void);                /* 0x20826 */
void road_advance(void);                 /* 0x1E53A */
void traffic_update(s16 unit);           /* 0x1DDEA */
void traffic_reset(void);                /* 0x1CCF6 */
void radar_zone(s32 pos);                /* 0x1EC1A */
void knob_update(void);                  /* 0x1CD72 */
void shift_gear(s16 delta);              /* 0x1EDCE */
void gate_select(u16 node);              /* 0x1EE76: from the 60 Hz VBL server in O mode */
void sim_gate_shift_vbl(u16 node);       /* 0x118B2's gate branch: gate_select + engine_update, as the
                                          * original runs both from the interrupt. Install it in
                                          * platform.h's vbl_gate_select for the drive. */
void car_record_copy(void);              /* 0x20D68 */
void stage_road_select(s16 stage);       /* 0x20E70 */
void lateral_physics(void);              /* 0x2484C */
void steering(void);                     /* 0x24924 */
void grip_check(void);                   /* 0x24A04 */
void rpm_from_speed(void);               /* 0x2076C */
void speed_from_rpm(void);               /* 0x20780 */
APTR road_record_at(s16 unit);           /* 0x1D32A: record of a road unit (0 outside the stage) */

/* The gear knob's sprites (drive_scene §4.8.4, inside 0x1CD72): knob_update owns the state that ends a shift,
 * the renderer owns the drawing and installs it here. Both may stay 0 (nothing is drawn, shifts still end). */
extern void (*knob_sprites_show)(s16 x, s16 y);   /* 0x1CDFE: sprites 4..7 to (x, y) */
extern void (*knob_sprites_hide)(void);           /* 0x1CF06: sprites 4..7 off screen */
