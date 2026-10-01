/* AMIGA: the Amiga features of the simulation (--amiga traffic, cops, wide-roads, signs; ../amiga_opts.h), as
 * rules the DOS simulation (sim.c, marked AMIGA there) applies instead of its own. The Amiga's own simulation
 * (port/amiga/spec/drive_sim.md in test-drive-sdl3) keeps its cars in fixed slots with lanes written back by the
 * renderer; the DOS one keeps lists, so its rules are carried over, not its code. Not bound by the faithful-port
 * rules (ENGINE.md). Speeds here are in the DOS units: sub-units (1/90 of a road unit) per tick, which is about
 * mph (the speed's high byte is the player's step). */
#include "game.h"

#include "sim_amiga.h"
#include "../amiga_opts.h"

/* The limits the speed signs show: DOS 25 / 50 / 60, Amiga 30 / 55 / 65 (D:0D86). idx is the DOS
 * speed_limit_idx: 0, 2 or 4. */
static u16 posted_limit(u16 idx)
{
    static const u16 DOS[3] = { 25, 50, 60 }, AMIGA[3] = { 30, 55, 65 };
    idx = (u16)(idx / 2 > 2 ? 2 : idx / 2);
    return amiga.signs ? AMIGA[idx] : DOS[idx];
}

static bool sign_seen;                      /* a speed-limit sign was passed on this stage */

void sim_amiga_stage_begin(void) { sign_seen = false; }
void sim_amiga_sign(void) { sign_seen = true; }

bool sim_amiga_rules(void) { return amiga.traffic || amiga.cops || amiga.signs || amiga.wide_roads; }

/* ---- wide roads: the record's first byte is the Amiga's width code (drive_scene.md §4.3 row_widths) */

double amiga_road_width(u16 pos)
{
    if (!amiga.wide_roads) return 1.0;
    for (int k = 0; k < 600; k++, pos--) {
        u8 b = DSB(pos);
        if (b == 0xFF && k > 0) break;                     /* the stage's end marker before it */
        if (b < 0x6E) {
            u8 w = DSB((u16)(DS_road_record_curve - 1 + (b << 2)));   /* byte 0 of the record */
            if (w != 0xFF) return (0x140 + 0x40 * (w & 15)) / (double)0x1C0;
        }
    }
    return 1.0;
}

/* The extra right half in car_x units (the road's half-width is 0x21C there: ROAD_HW 67.5 x 8). */
s16 sim_amiga_extra_right(void)
{
    double w = amiga_road_width(DSW(DS_road_pos));
    return (s16)((w - 1.0) * 0x21C);
}

/* ---- traffic */

/* The speed of a car of `type` (0..4; the 0x10.. / 0x18.. object codes) at the current limit. Amiga traffic
 * (drive_sim.md §4.8, §7): the limit; before the stage's first sign 42.7 / 50.7 / 58.7 / 38.7 / 30.7 mph by slot;
 * the same-direction slot 1 25 % faster; slot 0 at 3/4 of the limit on a wide road (road_edge_check). */
u16 sim_amiga_traffic_speed(u16 dos_speed, u16 type, bool samedir)
{
    if (!amiga.traffic) return amiga.signs ? posted_limit(DSW(DS_speed_limit_idx)) : dos_speed;
    static const u16 FIRST[5] = { 43, 51, 59, 39, 31 };
    u16 v = sign_seen ? posted_limit(DSW(DS_speed_limit_idx)) : FIRST[type < 5 ? type : 0];
    if (samedir && sign_seen && type == 1) v = (u16)(v + v / 4);
    if (samedir && type == 0 && amiga_road_width(DSW(DS_road_pos)) * 0x1C0 >= 0x258) v = (u16)(v - v / 4);
    return v;
}

/* ---- police */

/* The speed (speed word, mph << 8) from which a radar trap arms. DOS: 35 / 65 / 75 (the limit + 10 / 15 / 15).
 * Amiga cops: more than 15 mph over the limit (radar_zone 0x1EC1A). */
u16 sim_amiga_trap_speed(u16 dos_trap)
{
    u16 idx = DSW(DS_speed_limit_idx), limit = posted_limit(idx);
    if (amiga.cops) return (u16)((limit + 16) << 8);
    if (amiga.signs) return (u16)((limit + (idx == 0 ? 10 : 15)) << 8);
    return dos_trap;
}

/* The police car's speed when the chase starts: Amiga 108 + 4 mph per stage, constant (radar_zone). */
u16 sim_amiga_cop_speed(void) { return (u16)(108 + 4 * DSW(DS_g_stage)); }

/* True within 400 units of the stage's end, where the Amiga's police car never passes (traffic_update). */
bool sim_amiga_near_end(void)
{
    u16 p = DSW(DS_road_pos);
    for (int k = 0; k <= 0x190 + 0x28; k++)
        if (DSB((u16)(p + k)) == 0xFF) return true;
    return false;
}
