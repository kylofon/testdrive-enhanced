/* The drive's simulation — port of overlay 2's state updates, td 0x1CCF6-0x24AC8 spread over the overlay
 * (port/amiga/spec/drive_sim.md, all of §4). Called once per drive frame (5 VBL = 12 Hz on the NTSC clock the
 * port runs, port/amiga/README.md Decisions 1) by the drive flow, except gate_select/engine_update, which the
 * 60 Hz Ticks VBL server drives in O mode (§4.3).
 *
 * Almost all of the original is hand-written 68000 assembly with register arguments, so the code below follows
 * the disassembly instruction by instruction: `int` is 16 bits and wraps, the globals keep their D:xxxx
 * addresses in amem[] (so the traffic-slot bug writes the very words the original writes), `>>` on a signed
 * value is the 68000's `asr` (floor), `divs`/`divu` overflow leaves the destination unchanged and the
 * fixed-point formats are those of drive_sim §1 (speed 16.16 mph, road position 16.16 units, steering and
 * heading 16.16 degrees, rpm in the high word of D:191A).
 *
 * The simulation reads three groups of values the renderer computes: the row-0 road half-widths D:0D94/D:0D96
 * (drive_scene 0x1E5C6, called from the walker, not from here), the car_x snapshot D:289A and the traffic
 * lateral positions D:0CC2/D:0D28 (drive_scene §4.11). The drive loop must run the renderer's row/lane pass
 * every logic step even when nothing is drawn.
 *
 * Original bugs owned here (port/amiga/README.md): 1 (the traffic spawn slot index, §4.8) and 7 (the divs
 * overflow in lateral_physics, §4.5). Both are fixed unless g_original_bugs is set. */
#include "sim.h"

#include "scene.h"
#include "../aport.h"
#include "../asymbols.h"
#include "../platform/audio_drive.h"
#include "../platform/fixed.h"
#include "../platform/platform.h"

/* ---- tables and data of the code hunks (addresses of the loaded image, read in place) */
#define REC_TABLE     0x20F08u   /* 0x6E road records: [width code, curve, pitch, object] */
#define DIR_PEDAL     0x20D5Eu   /* 0x20D5E[10] = {0,1,1,0,-1,-1,-1,0,1,0}: joystick direction -> gas/brake */
#define DIR_STEER     0x24984u   /* 0x24984[10] = {0,0,1,1,1,0,-1,-1,-1,0}: direction -> steer left/right */
#define HAZ_EDGE      0x24A98u   /* 0x24A98[4] = {1, 0, -1, 0}: bump side per hazard */
#define HAZ_GRIP      0x24AA0u   /* 0x24AA0[4] = {0xCCCC, 0x9999, 0xCCCC, 0xBFFF}: 80/60/80/75 % grip */
#define STAGE_PAR     0x20EE0u   /* long[5] = {700, 1100, 1200, 1100, 1500} */
#define STAGE_PAR2    0x20EF4u   /* long[5] = {3000, 2800, 3500, 4000, 4500} */
#define BUG_LIST      0x1E89Au   /* drive_scene's 256-word bug list, cleared by traffic_reset */
#define D_gnob        0x257Cu    /* drive_scene: the `gnob` shape; +4/+6 = hot spot, +2 = height */

static const u32 ROAD_STREAMS[5] = { 0x210C0u, 0x218F2u, 0x22454u, 0x22F72u, 0x239E5u };

/* The gear-knob sprites of 0x1CD72 belong to drive_scene (§4.8.4); the state they carry (D:2832/D:2834 and
 * D:284C) decides when a shift completes, so it stays here and the renderer only installs the two drawing
 * steps. Both may be 0: the shift then still completes, only nothing is drawn. */
void (*knob_sprites_show)(s16 x, s16 y) = 0;
void (*knob_sprites_hide)(void) = 0;

static void shift_effects(void);
static void skid_drift(void);

/* ================================================================ helpers */

/* 0x1D31A: clamp the word at `addr` to [lo, hi] (the high bound is applied first, as the original does). */
static void clamp_word(s16 lo, s16 hi, u32 addr)
{
    if (hi < (s16)rd16(addr)) wr16(addr, (u16)hi);
    else if (lo > (s16)rd16(addr)) wr16(addr, (u16)lo);
}

/* 0x1D32A: the record of a road unit. Outside the stage (and at the 0xFF terminator, which also moves the
 * stage end back by 45) the answer is REC[0] = FF 00 00 00: no curve, no object. The original keeps the unit
 * in D7 and overwrites it on the terminator, which the caller then sees — hence the pointer. */
static APTR road_record_d7(s16 *d7)
{
    s16 u = *d7;
    u16 b = 0;
    if (u >= 0 && (u < (s16)D16(D_stage_end) || (s16)(u + 0x2D) < (s16)D16(D_stage_end))) {
        b = rd8(D32(D_road_stream) + (u32)(u16)u);
        if (b == 0xFF) {                                 /* the stage end moves to unit - 45 */
            *d7 = (s16)(u - 0x2D);
            SETD16(D_stage_end, (u16)(u - 0x2D));
            b = 0;
        }
    }
    return D32(D_road_records) + ((u32)(b & 0xFF) << 2);
}

APTR road_record_at(s16 unit)                            /* 0x1D32A, for callers that keep their own unit */
{
    s16 u = unit;
    return road_record_d7(&u);
}

/* ================================================================ engine (§4.4) */

void rpm_from_speed(void)                                /* 0x2076C: mulu.w, rpm = the high word */
{
    SETD32(D_rpm, (u32)(u16)((u32)D32(D_speed) >> 8) * (u32)D16(D_ratio));
}

void speed_from_rpm(void)                                /* 0x20780 */
{
    u32 dividend = (u32)((s32)(s16)D16(D_rpm) << 8);
    u16 r = D16(D_ratio) ? D16(D_ratio) : 1;
    u32 q = dividend / r;
    /* divu.w overflow leaves the dividend in D0, whose low word is then stored (never reached in practice). */
    SETD16(D_speed, (q > 0xFFFF) ? (u16)dividend : (u16)q);
}

/* 0x207A4: what happens when a shift completes — the clutch dump out of neutral, the 1st->2nd chirp, and the
 * rpm resynchronisation after every other shift. */
static void shift_effects(void)
{
    if (D16(D_shifting) == 0) {
        if ((s16)D16(D_prev_shifting) != 0) {            /* the shift completed since the last update */
            if ((s16)D16(D_gear) == 1 && (s16)D16(D_prev_gear) == 0
                && (s16)D16(D_pedal) >= 0 && (s16)D16(D_rpm) > 0xFA0) {
                SETD16(D_rpm, (u16)D16(D_rpm) >> 1);     /* clutch dump: half the revs go into speed */
                speed_from_rpm();
                SETD16(D_squeal_env, 0);
            } else {
                if ((s16)D16(D_gear) == 2 && (s16)D16(D_prev_gear) == 1 && (s16)D16(D_pedal) >= 0) {
                    u16 old = D16(D_rpm);
                    rpm_from_speed();
                    s16 d = (s16)(D16(D_rpm) - old);
                    SETD16(D_rpm, old);                  /* only the high word is put back */
                    if (d < (s16)0xF9C0) SETD16(D_squeal_env, 0xB);      /* a drop over 1600 rpm: chirp */
                }
                rpm_from_speed();
            }
        }
        SETD16(D_prev_gear, D16(D_gear));
    }
    SETD16(D_prev_shifting, D16(D_shifting));
}

void engine_update(void)                                 /* 0x20826 */
{
    s32 f;                                               /* D0: this frame's force */
    u16 d1_hi = 0;                                       /* see the drag note below */
    int clutch = 0;

    shift_effects();
    if (D16(D_shifting) != 0) clutch = 1;
    else if ((s16)D16(D_gear) == 0) {
        SETD32(D_speed, (s32)D32(D_speed) - 0x7FFF);     /* neutral loses speed; no clamp here */
        if ((s16)D16(D_pedal) <= 0) clutch = 1;
        else { SETD16(D_rpm, D16(D_rpm) + 0x1F4); f = 0; goto apply; }   /* free revving: +500 rpm */
    } else if ((s16)D16(D_pedal) > 0) {
        rpm_from_speed();
        u16 i = (u16)(D16(D_rpm) >> 7);                  /* the torque index is NOT clamped */
        u32 t = (u32)(u16)((u16)rd8(D32(D_car_torque) + i) << 4) * (u32)D16(D_ratio);
        if ((s16)D16(D_gear) == 1) {                     /* x1.5 in first, with a logical shift */
            u32 t2 = t << 1;
            d1_hi = (u16)(t2 >> 16);
            t = (t + t2) >> 1;
        }
        f = (s32)t;
        goto apply;
    } else if ((s16)D16(D_pedal) == 0) {
        engine_sound_update();                           /* in gear with no pedal: speed and rpm are held */
        return;
    } else {
        s32 s = (s32)D32(D_speed) - 0x42000;             /* brake: 4.125 mph per frame */
        SETD32(D_speed, s < 0 ? 0 : s);
        rpm_from_speed();
        f = 0;
        goto apply;
    }

    if (clutch) {                                        /* 0x20834: shifting, or neutral without gas */
        SETD16(D_rpm, (u16)(D16(D_rpm) - ((u16)D16(D_rpm) >> 5)));
        if ((s16)D16(D_pedal) < 0) {
            s32 s = (s32)D32(D_speed) - 0x42000;
            SETD32(D_speed, s < 0 ? 0 : s);
        }
        f = 0;
    }

apply:                                                   /* 0x208C2 */
    {
        /* The drag term is built with `swap d1 ; lsl.l #2,d1` on a register whose upper word is whatever was
         * left there: in first gear that is the high word of torque x 2 (kept here), otherwise it is the
         * caller's and the port uses 0. Either way it is under 0.004 mph (drive_sim §4.4). */
        u16 di = (u16)(((u16)D16(D_speed) >> 2) & 0x3F);
        u32 drag = ((((u32)rd8(DADDR(D_drag_table) + di)) << 16) | d1_hi) << 2;
        f -= (s32)drag;
        SETD32(D_speed, (s32)D32(D_speed) + (f >> 10));
    }
    if ((s16)D16(D_rpm) <= 0) SETD16(D_rpm, 0);          /* the fraction word is kept */
    if ((s32)D32(D_speed) >= 0) SETD32(D_advance, (u32)D32(D_speed) >> 6);
    else { SETD32(D_speed, 0); SETD32(D_advance, 0); }
    engine_sound_update();                               /* 0x26D4E, platform_audio */
}

/* ================================================================ lateral physics (§4.5) */

/* 0x249CC: the 6.0 lateral kicks, zeroed on the side the car is already close to. */
static void edge_kicks(s32 *kL, s32 *kR)
{
    *kL = *kR = 0x60000;
    s16 l = (s16)((s16)(0x15E - (s16)D16(D_car_half_left)) - (s16)D16(D_car_x));
    if (l > 0) *kL = 0;                                  /* near the left edge */
    else if ((s16)((s16)(D16(D_car_half_right) - 0x15E) - (s16)D16(D_car_x)) < 0) *kR = 0;
}

/* 0x2498E: step sizes — 1 degree per frame away from the road angle, up to 6 back toward it (which snaps). */
static void steer_steps(s32 *L, s32 *R, s32 *kL, s32 *kR)
{
    s32 d0 = 0x60000, d1 = 0x10000;
    edge_kicks(kL, kR);
    s32 d4 = (s32)D32(D_steer) - (s32)D32(D_steer_track);
    if (d4 > 0) {
        *kR = 0;
        if (d4 <= d1) d0 = d1;
        else if (d0 >= d4) d0 = d4;
    } else if (d4 < 0) {
        *kL = 0;
        d4 = -d4;
        if (d4 <= d1) d0 = d1;
        else {
            if (d0 >= d4) d0 = d4;
            s32 t = d0; d0 = d1; d1 = t;                 /* exg: the fast step is the one back to the angle */
        }
    } else d0 = d1;
    *L = d0;
    *R = d1;
}

void steering(void)                                      /* 0x24924 */
{
    s32 stepL, stepR, kickL, kickR;
    SETD32(D_steer_prev, D32(D_steer));
    steer_steps(&stepL, &stepR, &kickL, &kickR);
    s8 dir = (s8)rd8(DIR_STEER + (u32)(u16)D16(D_g_joyDir));
    if (dir < 0) {
        SETD32(D_steer, (s32)D32(D_steer) - stepL);
        SETD32(D_lat_force, (s32)D32(D_lat_force) - kickL);
    } else if (dir > 0) {
        SETD32(D_steer, (s32)D32(D_steer) + stepR);
        SETD32(D_lat_force, (s32)D32(D_lat_force) + kickR);
    }
    if ((s16)D16(D_steer_lock) != 0) {                   /* dead: D:0B36 is never written */
        s32 k = 0xC0000;
        s32 d = (s32)D32(D_steer_prev) - (s32)D32(D_steer);
        if (d != 0) {
            if (d > 0) k = -k;
            SETD32(D_car_x_new, (s32)D32(D_car_x_new) + k);
            SETD32(D_car_x, (s32)D32(D_car_x) + k);
        }
        SETD32(D_steer, D32(D_steer_track));
    }
    wheel_marker();                                      /* 0x247B2, drive_scene */
}

/* 0x24AA8: the skid turns the view by skid x car+00A >> 8. */
static void skid_drift(void)
{
    s16 s = (s16)D16(D_skid_amount);
    if (s == 0) return;
    s32 k = (s32)s * (s16)D16(D_car_skid_drift);         /* muls.w */
    if ((s16)D16(D_lat_sign) < 0) k = -k;
    SETD32(D_view_yaw, (s32)D32(D_view_yaw) + (k >> 8));
}

void grip_check(void)                                    /* 0x24A04 */
{
    SETD32(D_view_yaw, 0);
    u16 g = D16(D_car_grip);                             /* car +008 */
    SETD16(D_bump_edge, 0);
    if ((s16)D16(D_hazard_hit) != 0) {
        SETD16(D_hazard_hit, D16(D_hazard_hit) & 3);
        u16 h = D16(D_hazard_hit);
        SETD16(D_bump_edge, rd16(HAZ_EDGE + (u32)h * 2));
        g = (u16)(((u32)g * (u32)rd16(HAZ_GRIP + (u32)h * 2)) >> 16);   /* mulu.w; swap */
    }
    SETD16(D_lat_sign, D16(D_lat_force));                /* the high word, before the grip test */
    s16 a = abs_w((s16)D16(D_lat_force));
    s16 s = (s16)(a - (s16)g);
    if (s < 0) s = 0;
    if (s > 0x40) s = 0x40;
    SETD16(D_skid_amount, s);                            /* 0..0x40: the squeal volume */
    skid_drift();
    if (a >= (s16)g) {                                   /* skid: the force sticks at the limit */
        SETD16(D_lat_force, g);
        SETD16(D_lat_force + 2, 0);
        s32 sp = (s32)D32(D_speed) - 0x4000;             /* 0.25 mph per skidding frame */
        SETD32(D_speed, sp < 0 ? 0 : sp);
        rpm_from_speed();
        if ((s16)D16(D_lat_sign) < 0) SETD16(D_lat_force, (u16)(-(s16)D16(D_lat_force)));
    }
    SETD16(D_hazard_hit, 0);
}

/* 0x1DB36: the off-road crash tests, against the renderer's row-0 half-widths and its car_x snapshot. */
static void road_edge_check(void)
{
    if ((s16)((s16)(-(s16)D16(D_car_half_left)) - (s16)D16(D_r_lateral)) >= (s16)0xFF24) SETD16(D_crash, 1);
    if ((u16)D16(D_car_half_right) >= 0x258) {           /* a wide road: slot 0 drives 3/4 of the limit */
        s32 v = ((s32)((u32)D16(D_speed_limit) << 16)) >> 6;
        SETD32(D_sd_speed, v - (v >> 2));
    }
    if ((s16)(D16(D_car_half_right) - (s16)D16(D_r_lateral)) <= 0xC0) SETD16(D_crash, 1);
}

void lateral_physics(void)                               /* 0x2484C */
{
    SETD32(D_yaw_force, 0);
    SETD32(D_car_x_new, D32(D_car_x));
    s32 c = (s32)(D32(D_curve_acc) & 0xFFFF0000u);       /* clr.w: the integer part of the curve integral */
    SETD32(D_car_x_new, (s32)D32(D_car_x_new) - (c - (c >> 2)));   /* drift outward by 3/4 of it */
    SETD32(D_yaw_force, (s32)D32(D_yaw_force) + c);

    s32 t = c;
    if (c != 0) {
        s32 a = (s32)D32(D_advance) >> 8;
        if (a != 0) {                                    /* divs.w: the quotient must fit in a word */
            s32 q = c / (s32)(s16)a;
            if (q >= -0x8000 && q <= 0x7FFF) t = (s32)((u32)q << 16);
            else if (g_original_bugs) t = 0;             /* bug 7: V set, D0 unchanged; c's low word is 0,
                                                          * so the swap + clr.w that follow leave 0 */
            else t = (s32)((u32)(q < 0 ? 0x8000u : 0x7FFFu) << 16);   /* fixed: saturate the quotient */
        }
        t >>= 9;
    }
    SETD32(D_steer_track, t);                            /* = curve/2 degrees: cancels the drift exactly */

    SETD32(D_lat_force, 0);
    steering();
    SETD32(D_lat_force, (s32)D32(D_lat_force)
           + (((s32)(s16)((s32)D32(D_steer) >> 8) * (s16)((s32)D32(D_advance) >> 8)) << 3));   /* muls.w */
    SETD32(D_yaw_force, (s32)D32(D_yaw_force) - ((s32)D32(D_lat_force) >> 2));
    grip_check();
    {
        s32 lf = (s32)D32(D_lat_force) >> 2;
        SETD32(D_car_x_new, (s32)D32(D_car_x_new) + (lf - (lf >> 2)));
    }
    clamp_word(-1000, 1000, DADDR(D_car_x_new));
    SETD32(D_view_yaw, (s32)D32(D_view_yaw) + (((s32)D32(D_yaw_force) >> 4) + 1));

    if ((s16)D16(D_lane_hold) != 0) {                    /* pull over: steering has no effect */
        s16 d = (s16)((s16)D16(D_car_x) - 0x50);
        s16 m = abs_w(d);
        if (m >= 10) m = 10;
        if (d != 0) SETD16(D_car_x, (u16)((s16)D16(D_car_x) - (d > 0 ? m : (s16)-m)));
    } else if ((s16)D16(D_steer_lock) == 0) {            /* steer_lock is never set */
        SETD32(D_car_x, D32(D_car_x_new));
    }
    road_edge_check();
}

/* ================================================================ road objects (§4.8) */

/* 0x1E73A: the lane-marker band, 0x4C..0x70 around |v - 10|. */
static void marker_test(s16 v)
{
    v = (s16)(v - 0xA);
    if (v < 0) v = (s16)-v;
    if (v >= 0x4C && v <= 0x70) SETD16(D_bump_flag, 0xFFFF);
}

static void rumble_slow(void)                            /* 0x1E720 */
{
    s32 s = (s32)D32(D_speed) - 0x3000;
    SETD32(D_speed, s < 0 ? 0 : s);
    if (D16(D_shifting) == 0) rpm_from_speed();
}

/* 0x1E6A6: lane markers (every fourth unit) and the shoulders (the frame's first unit only). */
static void shoulder_bump(s16 d7)
{
    if ((s16)D16(D_speed) <= 0x17) return;
    if ((s16)D16(D_car_half_right) > 0x210) {            /* wide road: a second line of markers */
        if (d7 & 4) marker_test((s16)((s16)D16(D_car_x) * 2 - 0x1C0));
        marker_test((s16)((s16)D16(D_car_x) * 2));
    } else if (d7 & 4) {
        marker_test((s16)((s16)D16(D_car_x) * 2));
    }
    if ((s16)D16(D_first_unit) == 0) return;
    SETD16(D_first_unit, 0);
    if ((s16)((s16)((s16)D16(D_car_x) * 2 + (s16)D16(D_car_half_left)) + (s16)0xFF81) <= 0
        || (s16)((s16)(D16(D_car_half_right) - (s16)D16(D_car_x) * 2 - 0x50) + 0x4D) <= 0) {
        rumble_slow();
        SETD16(D_bump_flag, 0xFFFF);
    }
}

/* 0x1DB90: spawn a same-direction or oncoming car 40 units ahead of `d7`. */
static void traffic_spawn(APTR rec, s16 d7)
{
    u8 obj = rd8(rec + 3);
    if ((u8)(obj & 0xC0) > (u8)(rand16() & 0xC0)) return;     /* chance 100/75/50/25 % */
    /* Bug 1: the original indexes the slot with the whole object byte, so every object with chance bits
     * writes a stray word past the arrays and never spawns a car. The fix uses the masked code the branch
     * test above already works from (drive_sim §4.8). */
    u16 b = g_original_bugs ? (u16)obj : (u16)(obj & 0x3F);
    if (b >= 0x18) {
        if ((s16)D16(D_sd_enable) == 0 || (s16)D16(D_demo_mode) != 0) return;
        u32 a = DADDR(D_sd_pos) + (u32)(u16)((s16)(b - 0x18) * 4);
        if (rd16(a) != 0) return;
        wr16(a, (u16)(d7 + 0x28));
    } else {
        if ((s16)D16(D_on_enable) == 0 || (s16)D16(D_cop_tail_frames) != 0) return;
        u32 a = DADDR(D_on_pos) + (u32)(u16)((s16)(b - 0x10) * 4);
        if (rd16(a) != 0) return;
        wr16(a, (u16)(d7 + 0x28));
    }
}

/* 0x1DC08: a radar trap. The chance bits decide whether the zone starts. */
static void radar_zone_start(APTR rec)
{
    if ((u8)(rd8(rec + 3) & 0xC0) <= (u8)(rand16() & 0xC0)) SETD16(D_radar_zone_ctr, 1);
}

/* 0x1DCA0: a speed-limit sign sets the limit and every traffic speed. */
static void speed_sign(APTR rec, s16 k)
{
    (void)rec;
    s16 lim = (s8)rd8(DADDR(D_speed_limit_table) + (u32)(u16)(s16)(k - 5));
    SETD16(D_speed_limit, (u16)lim);
    s32 v = ((s32)((u32)(u16)lim << 16)) >> 6;
    for (int i = 0; i < 5; i++) {
        SETD32(D_sd_speed + i * 4, (u32)v);
        SETD32(D_on_speed + i * 4, (u32)v);
    }
    SETD32(D_sd_speed + 4, (s32)D32(D_sd_speed + 4) + (v >> 2));   /* slot 1 drives 25 % faster */
}

/* 0x1DC2E: the object of the unit the car has just entered. */
static void road_object(APTR rec, s16 d7)
{
    u16 k = (u16)(rd8(rec + 3) & 0x3F);
    if (k == 1) { radar_zone_start(rec); return; }
    if (k >= 5 && k <= 7) { speed_sign(rec, (s16)k); return; }
    if (k >= 0x10 && k <= 0x1D) { traffic_spawn(rec, d7); return; }
    if (k >= 0x20 && k <= 0x23) {                        /* hazard: the whole byte carries the lane */
        u16 obj = rd8(rec + 3);
        s16 d = (s16)((s16)(obj * 2) - (s16)D16(D_car_x) - 0xA);
        if (d < 0) d = (s16)-d;
        if ((u16)d >= 0x4C && (u16)d <= 0x70) SETD16(D_hazard_hit, obj);
    }
    shoulder_bump(d7);                                   /* every other object, and the hazards */
}

/* ================================================================ road advance (§4.7) */

static void curve_integral(APTR rec)                     /* 0x2481C */
{
    s16 cv = (s8)rd8(rec + 1);
    u16 fr = D16(D_seg_frac);
    s32 add;
    if (fr == 0) add = (s32)cv << 16;                    /* a whole unit */
    else if (cv == 0) return;
    else if (cv > 0) add = (s32)((u32)(u16)cv * (u32)fr);
    else add = -(s32)((u32)(u16)(s16)-cv * (u32)fr);
    SETD32(D_curve_acc, (s32)D32(D_curve_acc) + add);
}

static void heading_step(APTR rec)                       /* 0x1DCDC: scenery only */
{
    SETD32(D_heading, (s32)D32(D_heading) + ((((s32)(s8)rd8(rec + 1)) << 16) >> 3));
    while ((s16)D16(D_heading) < 0)    SETD16(D_heading, D16(D_heading) + 0x168);
    while ((s16)D16(D_heading) >= 0x168) SETD16(D_heading, D16(D_heading) - 0x168);
}

void road_advance(void)                                  /* 0x1E53A */
{
    SETD16(D_first_unit, 0xFFFF);
    SETD32(D_curve_acc, 0);
    if ((s16)D16(D_road_pos_done) >= 0) {
        s16 d7 = (s16)D16(D_road_pos_done);
        if (d7 == (s16)D16(D_road_pos)) {                /* still inside the same unit */
            s32 d = (s32)D32(D_road_pos) - (s32)D32(D_road_pos_done);
            if (d != 0) {
                SETD16(D_seg_frac, (u16)d);
                curve_integral(road_record_d7(&d7));
            }
        } else {
            s32 r = 0xFFFF - (s32)(u32)D16(D_road_pos_done + 2);   /* 0xFFFF, not 0x10000 */
            if (r != 0) {
                SETD16(D_seg_frac, (u16)r);
                curve_integral(road_record_d7(&d7));
            }
            do {
                SETD16(D_road_pos_done, D16(D_road_pos_done) + 1);
                d7 = (s16)D16(D_road_pos_done);
                SETD16(D_seg_frac, 0);
                APTR rec = road_record_d7(&d7);          /* may push d7 back at the 0xFF terminator */
                road_object(rec, d7);
                heading_step(rec);
                if (d7 == (s16)D16(D_road_pos)) SETD16(D_seg_frac, D16(D_road_pos + 2));
                curve_integral(rec);
            } while (d7 < (s16)D16(D_road_pos));
        }
    }
    SETD32(D_road_pos_done, D32(D_road_pos));
}

/* ================================================================ radar and police spawn (§4.9) */

void radar_zone(s32 pos)                                 /* 0x1EC1A */
{
    if ((s16)D16(D_radar_zone_ctr) == 0) return;
    SETD32(D_radar_zone_ctr, (s32)D32(D_radar_zone_ctr) + (s32)((u32)D32(D_advance) >> 5));
    if ((s16)D16(D_radar_zone_ctr) < 5) return;          /* the speed is read 2..3 units into the zone */
    if ((s16)((s16)D16(D_speed) - (s16)D16(D_speed_limit) - 0xF) > 0
        && (s16)D16(D_demo_mode) == 0 && D32(D_cop_pos) == 0) {
        SETD32(D_cop_pos, (u32)pos);
        SETD16(D_cop_pos, (u16)(D16(D_cop_pos) - 0x1E));               /* 30 units behind */
        SETD32(D_cop_speed, 0x1B000);                                  /* 108 mph ... */
        SETD32(D_cop_speed, (s32)D32(D_cop_speed)
               + ((((s32)((u32)D16(D_g_stage) << 16))) >> 4));         /* ... + 4 mph per stage */
    }
    if ((s16)D16(D_radar_zone_ctr) >= 7) {
        SETD32(D_radar_zone_ctr, 0);
        SETD16(D_radar_blink, 0x32);
    }
}

/* ================================================================ traffic and police (§4.10) */

void traffic_update(s16 unit_unused)                     /* 0x1DDEA: the argument is unused, as in the original */
{
    (void)unit_unused;
    s16 unit = (s16)D16(D_road_pos);

    for (int s = 0; s < 6; s++) {                        /* slot 5 is the police car */
        u32 pos = DADDR(D_sd_pos) + (u32)s * 4;
        u32 lane = DADDR(D_sd_lane) + (u32)s * 4;
        if (rd16(pos) == 0) continue;
        s32 np = (s32)rd32(pos) + (s32)rd32(DADDR(D_sd_speed) + (u32)s * 4);

        for (int j = 0; j < 6; j++) {                   /* cars ahead within 7 units, same lane band */
            u32 jpos = DADDR(D_sd_pos) + (u32)j * 4;
            u32 jlane = DADDR(D_sd_lane) + (u32)j * 4;
            s32 d = np - (s32)rd32(jpos);
            if (d >= 0 || d + 0x70000 < 0) continue;
            s16 dl = (s16)((s16)rd16(jlane) - (s16)rd16(lane));
            if (dl > 0xE6 || dl < (s16)0xFF1A) continue;
            if (s == 5 && (u16)D16(D_cop_mode) < 10) {  /* the chasing police car pushes it aside */
                u32 jdodge = DADDR(D_sd_dodge) + (u32)j * 4;
                wr16(jdodge, (u16)(rd16(jdodge) + 0x38));
                wr16(jpos, (u16)(rd16(jpos) - 1));
                SETD16(D_police_slide, D16(D_police_slide) - 0x38);   /* = sd_dodge[5] */
            } else {                                    /* queue 7 units behind */
                np = (s32)rd32(jpos) - 0x70000;
                wr32(pos, (u32)np);
            }
        }

        wr16(lane + 2, 0);
        int skip = 0;
        if (s == 5) skip = (D16(D_cop_mode) == 10);
        else {
            s16 dx = (s16)((s16)D16(D_car_x) - (s16)rd16(lane));
            skip = (dx > 0xE6 || dx < (s16)0xFF1A);
        }
        if (!skip) {
            s32 r = np - (s32)D32(D_road_pos);
            if (r <= 0x60000 && r >= -0xC0000) {
                if (r < 0x20000) {                      /* it never hits the player from behind */
                    wr16(lane + 2, 1);
                    np = (s32)D32(D_road_pos) - 0xC0000;
                } else if ((s16)D16(D_autopilot) != 0) {
                    SETD32(D_road_pos, np - 0x60000);   /* the autopilot is held back instead */
                } else {
                    SETD16(D_crash, 1);
                    if (s == 5) SETD16(D_crash, 2);     /* the police car: game over */
                }
            }
        }
        wr32(pos, (u32)np);
        if (np == 0) wr16(pos, (u16)(rd16(pos) + 2));
        s16 rel = (s16)((s16)rd16(pos) - unit);
        if (rel >= 0x46) wr32(pos, 0);                  /* 70 ahead */
        if ((s16)(rel + 0x28) < 0) wr32(pos, 0);        /* 40 behind */
    }

    /* police FSM (slot 5) */
    int to_oncoming = 0;
    if ((s16)D16(D_cop_pos) == 0) {
        SETD16(D_cop_mode, 0); SETD16(D_ticket_frames, 0);
        SETD16(D_cop_tail_frames, 0); SETD16(D_cop_timer, 0);
        to_oncoming = 1;
    } else if ((s16)D16(D_ticket_frames) >= 0x1E) {     /* the ticket is written: it drives away */
        SETD16(D_radar_zone_ctr, 0);
        SETD32(D_cop_speed, (s32)D32(D_cop_speed) + 0x600);
        to_oncoming = 1;
    } else {
        int pass = 1;
        if ((s16)D16(D_cop_timer) == 0) {
            if ((s16)D16(D_cop_tailing) == 0) {
                SETD16(D_cop_tail_frames, 0); SETD16(D_cop_timer, 0);
                to_oncoming = 1;
            } else {
                SETD16(D_cop_tail_frames, D16(D_cop_tail_frames) + 1);
                if ((u16)((s16)D16(D_stage_end) - unit) <= 0x190) to_oncoming = 1;   /* never near the end */
                else if ((u16)D16(D_cop_tail_frames) < 0x46) to_oncoming = 1;        /* tails 70 frames */
            }
            pass = !to_oncoming;
        }
        if (pass) {
            SETD16(D_cop_timer, D16(D_cop_timer) + 1);
            if ((u16)D16(D_cop_timer) <= 0x3C) {        /* passing: from 14 behind to 15 ahead */
                SETD16(D_cop_mode, 10);
                s16 o = (s16)(((s16)(D16(D_cop_timer) - 0x1E)) >> 1);
                if (o > 15) o = 15;
                if (o <= -12) o = -14;
                SETD16(D_cop_pos, (u16)unit);
                SETD16(D_cop_pos, (u16)(D16(D_cop_pos) + o));
                SETD32(D_cop_speed, D32(D_advance));
            } else {
                if ((s16)(unit + 15) <= (s16)D16(D_cop_pos)) SETD16(D_cop_pos, (u16)(unit + 15));
                if ((u16)D16(D_cop_timer) > 0x5A) {     /* braking in front of the player */
                    SETD16(D_cop_mode, 15);
                    SETD32(D_cop_speed, (s32)D32(D_cop_speed) - 0x600);
                    if ((s32)D32(D_cop_speed) <= 0) {
                        SETD32(D_cop_speed, 0);
                        if ((s32)D32(D_advance) <= 0) SETD16(D_ticket_frames, D16(D_ticket_frames) + 1);
                    }
                }
            }
        }
    }

    for (int s = 0; s < 5; s++) {                       /* oncoming cars */
        u32 pos = DADDR(D_on_pos) + (u32)s * 4;
        u32 lane = DADDR(D_on_lane) + (u32)s * 4;
        if (rd32(pos) == 0) continue;
        s32 np = (s32)rd32(pos) - (s32)rd32(DADDR(D_on_speed) + (u32)s * 4);
        for (int j = 0; j < 5; j++) {
            u32 jpos = DADDR(D_on_pos) + (u32)j * 4;
            s32 d = np - (s32)rd32(jpos);
            if (d > 0 && d - 0x70000 < 0) { np = (s32)rd32(jpos) + 0x70000; wr32(pos, (u32)np); }
        }
        s16 dx = (s16)((s16)D16(D_car_x) - (s16)rd16(lane));
        if (dx <= 0xE6 && dx >= (s16)0xFF1A) {
            s32 r = np - (s32)D32(D_road_pos);
            if (r <= 0x60000 && r >= 0x20000) SETD16(D_crash, 1);
        }
        wr32(pos, (u32)np);
        if (np == 0) wr16(pos, (u16)(rd16(pos) - 2));
        if ((s16)((s16)rd16(pos) + 0x28 - unit) <= 0) wr32(pos, 0);
    }
}

void traffic_reset(void)                                 /* 0x1CCF6 */
{
    for (int i = 0; i < 0x100; i++) wr16(BUG_LIST + (u32)i * 2, 0);   /* drive_scene's bug list */
    for (int i = 0; i < 6; i++) SETD32(D_sd_pos + i * 4, 0);
    for (int i = 0; i < 5; i++) SETD32(D_on_pos + i * 4, 0);
    SETD16(D_ticket_frames, 0);
    SETD16(D_cop_tail_frames, 0);
    SETD16(D_cop_timer, 0);
    SETD16(D_cop_mode, 0);
    SETD32(D_radar_zone_ctr, 0);
}

/* ================================================================ shifting (§4.3) */

void shift_gear(s16 delta)                               /* 0x1EDCE */
{
    SETD16(D_gear, D16(D_gear) + (u16)delta);
    if ((s16)D16(D_gear) < 0) SETD16(D_gear, 0);
    if ((s16)D16(D_gear) > (s16)D16(D_car_num_gears)) SETD16(D_gear, D16(D_car_num_gears));
    u32 g = (u32)(u16)((s16)D16(D_gear) * 4);
    SETD16(D_knob_target_x, (u16)(rd16(D32(D_car_knob_xy) + g) + 0x80 - rd16(D32(D_gnob) + 4)));
    SETD16(D_knob_target_y, (u16)(rd16(D32(D_car_knob_xy) + g + 2) + 0x2C - rd16(D32(D_gnob) + 6)));
    SETD16(D_ratio, rd16(D32(D_car_ratios) + (u32)(u16)((s16)D16(D_gear) * 2)));   /* also in neutral */
    if (delta != 0) {
        SETD8(D_gearbox_show + 1, 0xFF);
        SETD8(D_gearbox_show, 0xFF);
        SETD16(D_shifting, 1);                           /* set even if the clamp left the gear alone */
    } else {
        SETD16(D_knob_x, D16(D_knob_target_x));
        SETD16(D_knob_y, D16(D_knob_target_y));
        SETD16(D_shifting, 0);
    }
}

void gate_select(u16 node)                               /* 0x1EE76: gate (O) mode */
{
    SETD16(D_gear, (u16)(s16)(s8)rd8(D32(D_car_node_gear) + node));
    u32 n = (u32)(u16)((s16)node * 4);
    SETD16(D_knob_target_x, (u16)(rd16(D32(D_car_gate_xy) + n) + 0x80 - rd16(D32(D_gnob) + 4)));
    SETD16(D_knob_target_y, (u16)(rd16(D32(D_car_gate_xy) + n + 2) + 0x2C - rd16(D32(D_gnob) + 6)));
    SETD16(D_ratio, rd16(D32(D_car_ratios) + (u32)(u16)((s16)D16(D_gear) * 2)));
    SETD16(D_gate_col, node & 3);
    SETD16(D_gate_row, (u16)(((s16)node) >> 2));
    if (D16(D_knob_x) != D16(D_knob_target_x) || D16(D_knob_y) != D16(D_knob_target_y)) SETD16(D_shifting, 1);
}

/* The Ticks VBL server's gate branch (0x118B2): the original selects the node and then runs engine_update
 * from the interrupt, so in O mode the neutral decay and the rpm decay run at 60 Hz (§4.3, quirk).
 * platform/input.c calls this through vbl_gate_select. */
void sim_gate_shift_vbl(u16 node)
{
    gate_select(node);
    engine_update();
}

void knob_update(void)                                   /* 0x1CD72 */
{
    /* The knob moves only once the renderer has put the gearbox on both buffers. */
    if (D8(D_gearbox_shown) != 0 && D8(D_gearbox_shown + 1) != 0) {
        s16 d = (s16)(D16(D_knob_target_x) - D16(D_knob_x));
        if (d > 4) SETD16(D_knob_x, D16(D_knob_x) + 4);
        else if (d < -4) SETD16(D_knob_x, D16(D_knob_x) - 4);
        else SETD16(D_knob_x, D16(D_knob_target_x));
        d = (s16)(D16(D_knob_target_y) - D16(D_knob_y));
        if (d > 8) SETD16(D_knob_y, D16(D_knob_y) + 8);
        else if (d < -8) SETD16(D_knob_y, D16(D_knob_y) - 8);
        else SETD16(D_knob_y, D16(D_knob_target_y));

        if (D16(D_knob_x) == D16(D_knob_shown_x) && D16(D_knob_y) == D16(D_knob_shown_y)
            && (s16)D16(D_knob_visible) != 0) {                 /* the knob has arrived: the shift is done */
            if (D16(D_shifting) != 0) SETD16(D_shifting, 0);
            if (D16(D_gearbox_hide_delay) != 0) SETD16(D_gearbox_hide_delay, D16(D_gearbox_hide_delay) - 1);
            if (D16(D_gearbox_hide_delay) == 0 && (s16)D16(D_gearbox_always) == 0) {
                SETD8(D_gearbox_show + 1, 0);
                SETD8(D_gearbox_show, 0);
                SETD16(D_knob_shown_x, 0);
            }
        } else {
            if (knob_sprites_show) knob_sprites_show((s16)D16(D_knob_x), (s16)D16(D_knob_y));
            SETD16(D_knob_shown_y, D16(D_knob_y));
            SETD16(D_knob_shown_x, D16(D_knob_x));
            SETD16(D_gearbox_hide_delay, 7);
            SETD16(D_knob_visible, 1);
        }
    }
    if ((s16)D16(D_knob_visible) != 0 && (D8(D_gearbox_show) == 0 || D8(D_gearbox_show + 1) == 0)) {
        if (knob_sprites_hide) knob_sprites_hide();      /* sprites 4..7 off screen */
        SETD16(D_knob_visible, 0);
    }
}

/* ================================================================ controls (§4.2) */

/* 0x20C78 / 0x20C94: the demo's upshift and the stage-end downshift. Such a frame does not move the car. */
static void auto_shift(s16 delta)
{
    SETD16(D_pedal, 0);
    SETD16(D_gearbox_hide_delay, 7);
    SETD16(D_gearbox_show, 0xFFFF);
    shift_gear(delta);
}

/* 0x20C1E: fire held — the clutch is in and the stick shifts, one gear per deflection. No lateral physics
 * this frame, so the curve drift is lost. */
static void controls_fire(void)
{
    SETD16(D_pedal, 0);
    SETD16(D_gearbox_hide_delay, 7);
    SETD16(D_gearbox_show, 0xFFFF);
    if (((s16)D16(D_fire_prev) == 0 || (s16)D16(D_dir_prev) == 0) && (s16)D16(D_gate_mode) == 0) {
        SETD16(D_dir_prev, (u16)joy_dir());
        s16 p = (s8)rd8(DIR_PEDAL + (u32)(u16)D16(D_g_joyDir));
        if (p != 0) shift_gear(p);                       /* up = +1, down = -1 */
    }
    SETD16(D_fire_prev, 1);
    SETD16(D_dir_prev, (u16)joy_dir());
    SETD32(D_road_pos, (s32)D32(D_road_pos) + (s32)D32(D_advance));
}

void sim_controls(void)                                  /* 0x20CB0 */
{
    s16 d0;

    engine_update();                                     /* uses last frame's pedal */
    SETD16(D_pedal, 0);

    if ((s16)D16(D_car_rev_limit) < (s16)D16(D_rpm)) {   /* over the rev limit */
        SETD16(D_overrev_frames, D16(D_overrev_frames) + 1);
        if ((u16)D16(D_overrev_frames) >= 10) {
            SETD16(D_crash, 1);                          /* a blown engine after 10 frames */
            SETD16(D_overrev_frames, 0);
        }
    } else {
        SETD16(D_overrev_frames, 0);
    }

    if ((s16)D16(D_near_end) != 0) {                     /* autopilot: brake, then downshift */
        if (D16(D_shifting) == 0 && (s16)D16(D_rpm) <= 0xFA0) { auto_shift(-1); return; }
        d0 = 5;
        SETD16(D_g_joyDir, 5);
    } else if ((s16)D16(D_demo_mode) != 0) {
        if (D16(D_shifting) == 0 && (s16)D16(D_gear) < (s16)D16(D_car_num_gears)
            && (s16)D16(D_rpm) > 0x1770) { auto_shift(1); return; }
        d0 = 1;
        SETD16(D_g_joyDir, 1);
    } else {
        if (joy_fire() != 0) { controls_fire(); return; }
        d0 = joy_dir();
    }

    SETD16(D_dir_prev, (u16)d0);
    lateral_physics();
    {
        s16 p = (s8)rd8(DIR_PEDAL + (u32)(u16)D16(D_g_joyDir));
        if (p != 0) SETD16(D_pedal, (u16)p);             /* set after the physics: the next frame uses it */
    }
    SETD16(D_fire_prev, 0);
    SETD32(D_road_pos, (s32)D32(D_road_pos) + (s32)D32(D_advance));
}

/* ================================================================ car and stage setup */

void car_record_copy(void)                               /* 0x20D68: the car record -> D:1924.. (FORMATS.md) */
{
    APTR a0 = D32(D_g_carRecord);
    SETD16(D_car_num_gears, rd16(a0 + 0x000));                    /* num_gears */
    SETD16(0x1926, rd16(a0 + 0x002));
    SETD16(D_car_rev_limit, rd16(a0 + 0x004));                    /* rev limit */
    SETD16(0x192A, rd16(a0 + 0x006));
    SETD16(D_car_grip, rd16(a0 + 0x008));                    /* grip */
    SETD16(D_car_skid_drift, rd16(a0 + 0x00A));                    /* skid drift */
    SETD16(0x1930, rd16(a0 + 0x00C));
    SETD16(0x1932, rd16(a0 + 0x00E));
    SETD16(0x1934, rd16(a0 + 0x010));
    SETD32(D_car_ratios, a0 + 0x012);                          /* gear ratios */
    SETD32(D_car_knob_xy, a0 + 0x020);                          /* knob positions */
    SETD32(D_car_gate_xy, a0 + 0x03C);                          /* gate positions */
    SETD32(D_car_gate_next, a0 + 0x07C);                          /* gate transition table */
    SETD32(D_car_node_gear, a0 + 0x10C);                          /* gate node -> gear */
    SETD32(D_car_torque, a0 + 0x11C);                          /* torque curve */
    SETD16(0x194E, rd16(a0 + 0x16C));
    if (rd16(a0 + 0x16C) == 0) {                         /* the long (analog dashboard) layout */
        SETD32(0x1950, a0 + 0x16E);
        SETD16(0x1954, rd16(a0 + 0x17A));
        SETD16(0x1956, rd16(a0 + 0x17C));
        SETD16(0x1958, rd16(a0 + 0x17E));
        SETD16(0x195A, rd16(a0 + 0x180));
        SETD32(0x195C, rd32(a0 + 0x182));
        SETD32(0x1960, a0 + 0x31C);
        SETD16(0x1964, rd16(a0 + 0x324));
        SETD16(0x1966, rd16(a0 + 0x326));
        SETD16(0x1968, rd16(a0 + 0x328));
        SETD16(0x196A, rd16(a0 + 0x32A));
        SETD32(0x196C, rd32(a0 + 0x32C));
    } else {                                             /* the short (digital) layout, e.g. Vette */
        SETD8(0x1970, rd8(a0 + 0x16E));
        SETD8(0x1971, rd8(a0 + 0x16F));
        SETD8(0x1972, rd8(a0 + 0x170));
        SETD8(0x1973, rd8(a0 + 0x171));
        SETD16(0x1974, rd16(a0 + 0x172));
        SETD16(0x1976, rd16(a0 + 0x174));
        SETD16(0x1978, rd16(a0 + 0x176));
        SETD16(0x197A, rd16(a0 + 0x178));
        SETD32(0x197C, a0 + 0x17A);
        SETD32(0x1980, a0 + 0x328);
    }
}

void stage_road_select(s16 stage)                        /* 0x20E70 */
{
    SETD32(D_road_records, REC_TABLE);
    u32 stream = ROAD_STREAMS[(stage >= 0 && stage <= 3) ? (int)stage : 4];
    SETD16(D_g_scoreFactor, (u16)(stage + 4));
    s16 i = 0;
    do i++; while (rd8(stream + (u32)(u16)i) != 0xFF);   /* the search starts at index 1 */
    SETD16(D_stage_end, (u16)(i - 0x2D));
    SETD32(D_road_stream, stream);
    u32 k = (u32)(u16)(s16)(stage * 4);
    SETD32(D_g_parTime, rd32(STAGE_PAR + k));
    SETD32(D_g_stageConstB, rd32(STAGE_PAR2 + k));
}
