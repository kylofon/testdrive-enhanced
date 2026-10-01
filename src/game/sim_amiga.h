#pragma once
/* AMIGA: the Amiga features of the simulation (sim_amiga.c). */
#include "../mem.h"

void   sim_amiga_stage_begin(void);       /* run_stage: a new stage, no sign passed yet */
void   sim_amiga_sign(void);              /* sim_advance_unit: a speed-limit sign was passed */
bool   sim_amiga_rules(void);             /* any feature that changes the simulation's numbers */

/* The right half-width at road unit pos as a multiple of the usual (1 without Amiga wide roads, up to 2). */
double amiga_road_width(u16 pos);
/* The extra right half at the car's unit, in car_x units (0 on a usual road). */
s16    sim_amiga_extra_right(void);

u16  sim_amiga_traffic_speed(u16 dos_speed, u16 type, bool samedir);
u16  sim_amiga_trap_speed(u16 dos_trap);
u16  sim_amiga_cop_speed(void);
bool sim_amiga_near_end(void);
