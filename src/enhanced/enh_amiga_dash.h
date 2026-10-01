#pragma once
/* AMIGA dashboards: the Amiga cockpit as a layer over the enhanced view (enh_amiga_dash.c). */
#include "../mem.h"

/* At stage start: the cockpit of the car being driven. False if the Amiga disk lacks it (then the DOS one). */
bool amiga_dash_begin(void);
void amiga_dash_end(void);
bool amiga_dash_active(void);
/* The cockpit over rows 0..199, ARGB (alpha 0 = the road view shows), recomposed from the game's state. */
const u32 *amiga_dash_layer(void);
