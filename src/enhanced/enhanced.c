/* Enhanced renderer. Not bound by the faithful-port rules in ENGINE.md.
 *
 * The original frame (projection, road buffer, mirror, overlays, dashboard) still runs every frame, so
 * game state and the cockpit behave as before. This module renders the road window (rows 19..110) again
 * on the host side and lays it over the EGA frame through gfx_set_overlay():
 *   - 120 road rows instead of 40, using the original geometry (depth Z = 34 + 15 t, road half-width
 *     10125 / Z px, x = 120 + 150 X / Z, y = 70 - 180 Y / dist), with distance haze;
 *   - smooth motion: the car's progress inside the current road unit (DS:0912) moves the rows, and the
 *     lateral position and heading, which the simulation only updates per unit, are smoothed;
 *   - output at a multiple of 320x200 (gfx_output_scale), supersampled 2x2 (4x4 at scale 1), rendered in
 *     horizontal bands on the host worker pool; a depth buffer for sprite occlusion (crests, the cliff);
 *   - a sky with mountains and a valley floor below a lowered horizon on the open side, the original's
 *     slanted rock face on the other side and a hillside under the left road edge;
 *   - the original sprites decoded from their plane data and scaled with distance;
 *   - Test Drive II's distance and time readout (top right), 8 Hz emulation of the frame-counted gear-box
 *     delay, own crash sequence.
 * Pixels that the original draws over the road window are left to the EGA image: the mirror, the
 * ticket and the status text (computed coverage) and anything drawn on screen after the road buffer
 * was presented (VRAM compared against a snapshot taken at that moment).
 */
#include "enhanced.h"
#include "../host.h"
#include "../symbols.h"
#include "../game/game.h"
#include "../platform/gfx.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define WIN_Y0    19                /* road window rows 19..110 */
#define WIN_Y1    111
#define WIN_H     (WIN_Y1 - WIN_Y0)
#define NROWS     120               /* road rows drawn (the original draws 40) */
#define HORIZON   82.0              /* horizon row on the open side (eye level is row 70) */
#define VALLEY_H  600.0             /* camera height above the valley floor, road Y units */
#define RIM_H     7.0               /* dark rim under the left road edge, road Y units */
#define HILL_LEAN (150.0 / 180.0)   /* hillside under the left edge: 1:1 slope, screen px per px */
#define CLIFF_LEAN (23.0 / 114.0)   /* slant of the rock face, screen px per px (the clfo sprite) */
#define CLIFF_JAG 2.5               /* deepest notch in the rock face's edge, screen px */
#define JAG_FOOT  6.0               /* notches fade in over this height from the road edge, screen px */
#define ROAD_HW   67.5              /* road half-width in X units (10125 / 150) */
#define Z_EPS     18.0f             /* sprite depth tolerance (a bit more than one road unit) */
#define Z_INF     1e30f
#define REC(b, k) DSB((u16)(0x2B70 + ((u16)(b) << 2) + (k)))   /* road record table */

enum { OP_COPY, OP_OR, OP_AND, OP_XOR };

static bool active, dirty;

/* Resolution: output OK x (320 x WIN_H), rendered at SSF x SSF samples per output pixel, so Q internal
 * pixels per original pixel. Buffers are (re)allocated by ensure_buffers(). */
static int OK, SSF, Q, OW, OH, RW, RH;

static u32   *col;                  /* RW x RH */
static u8    *mat;                  /* 3-bit colour index the original would hold (for sprite ops) */
static float *zbuf;
static bool  *row_claimed;          /* RH: scanline already has its nearest road row */
static int   *row_filled;           /* RH: pixels written on the scanline (each at most once) */
static int   *row_suffix;           /* RH: columns [row_suffix, RW) of the scanline are all written */
static u32   *sky;                  /* RH */
static double *cliff_jag, *hill_jag;   /* RH: notch depth of the rock face's / hillside's edge, screen px */
static float *far_top, *near_top, *snow;   /* RW: mountain outlines */
static u32   *valley, *soft, *out;  /* OW x OH */
static u8    cover[320 * 200];      /* 1 = show the EGA image */
static u8    vram_snap[4][40 * WIN_H];

/* ------------------------------------------------------------------------------------------------ */
/* colours                                                                                          */

typedef struct { float r, g, b; } Rgb;

static Rgb hex(u32 v) { return (Rgb){ (v >> 16 & 255) / 255.0f, (v >> 8 & 255) / 255.0f, (v & 255) / 255.0f }; }
static Rgb mix(Rgb a, Rgb b, float t) { return (Rgb){ a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t }; }
static u32 pack(Rgb c)
{
    int r = (int)(c.r * 255.0f + 0.5f), g = (int)(c.g * 255.0f + 0.5f), b = (int)(c.b * 255.0f + 0.5f);
    r = r < 0 ? 0 : r > 255 ? 255 : r;
    g = g < 0 ? 0 : g > 255 ? 255 : g;
    b = b < 0 ? 0 : b > 255 ? 255 : b;
    return (u32)(r << 16 | g << 8 | b);
}
static u32 blend(u32 a, u32 b, float t)
{
    int it = (int)(t * 256.0f);
    int ar = (int)(a >> 16 & 255), ag = (int)(a >> 8 & 255), ab = (int)(a & 255);
    int br = (int)(b >> 16 & 255), bg = (int)(b >> 8 & 255), bb = (int)(b & 255);
    return (u32)((ar + (((br - ar) * it) >> 8)) << 16 | (ag + (((bg - ag) * it) >> 8)) << 8
                 | (ab + (((bb - ab) * it) >> 8)));
}

static const u32 C_HAZE = 0xC6E2EC, C_SKY_TOP = 0x4E9BDE, C_SKY_LOW = 0xCBEAF4;
static const u32 C_MTN_FAR = 0x8FAAC2, C_MTN_NEAR = 0x6A8599;
static const u32 C_FIELD_A = 0x4F7A3A, C_FIELD_B = 0x7F9A4C, C_FIELD_C = 0x9A8E58, C_WOOD = 0x2F5530;
static const u32 C_RIM = 0x241C16, C_HILL_TOP = 0x4A4630, C_HILL = 0x5F6A3A;
static const u32 C_ROAD_A = 0x1E1E22, C_ROAD_B = 0x27272C, C_SHLD_A = 0x5C5C5C, C_SHLD_B = 0x4C4C4C;
static const u32 C_DASH = 0xF2F2F2;

#define ZLUT_STEP 4.0
#define ZLUT_N    512
static float haze_lut[ZLUT_N];
static u16   to_linear[256];              /* sRGB byte -> linear 0..4095 */
static u8    to_srgb[4096];

static void init_luts(void)
{
    for (int i = 0; i < ZLUT_N; i++) {
        double z = i * ZLUT_STEP, a = (z - 300.0) / 1700.0;
        a = a < 0 ? 0 : a > 1 ? 1 : a;
        haze_lut[i] = (float)(0.6 * pow(a, 0.9));
    }
    for (int i = 0; i < 256; i++) {
        double c = i / 255.0;
        c = c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4);
        to_linear[i] = (u16)(c * 4095.0 + 0.5);
    }
    for (int i = 0; i < 4096; i++) {
        double c = i / 4095.0;
        c = c <= 0.0031308 ? c * 12.92 : 1.055 * pow(c, 1.0 / 2.4) - 0.055;
        to_srgb[i] = (u8)(c * 255.0 + 0.5);
    }
}

static int zlut_idx(double z)
{
    int i = (int)(z / ZLUT_STEP);
    return i < 0 ? 0 : i >= ZLUT_N ? ZLUT_N - 1 : i;
}
static float haze_of(double z) { return haze_lut[zlut_idx(z)]; }

static u32 hazed(u32 c, double z) { return blend(c, C_HAZE, haze_of(z)); }
/* The road keeps more contrast in the distance than the scenery around it. */
static u32 road_hazed(u32 c, double z) { return blend(c, C_HAZE, 0.65f * haze_of(z)); }

/* ------------------------------------------------------------------------------------------------ */
/* noise                                                                                            */

static u32 hash32(u32 x)
{
    x ^= x >> 16; x *= 0x7FEB352Du;
    x ^= x >> 15; x *= 0x846CA68Bu;
    x ^= x >> 16;
    return x;
}
static double h01(u32 x) { return (hash32(x) & 0xFFFFFF) / 16777216.0; }
static double smooth(double t) { return t * t * (3.0 - 2.0 * t); }

/* 1-D value noise, periodic over `period` cells */
static double noise1(double u, long period, u32 seed)
{
    double fl = floor(u);
    long a = (((long)fl % period) + period) % period, b = (a + 1) % period;
    double t = smooth(u - fl);
    double va = h01((u32)a * 0x9E3779B1u + seed), vb = h01((u32)b * 0x9E3779B1u + seed);
    return va + (vb - va) * t;
}

static double noise2(double x, double y, u32 seed)
{
    double fx = floor(x), fy = floor(y);
    u32 ix = (u32)(s32)fx, iy = (u32)(s32)fy;
    double tx = smooth(x - fx), ty = smooth(y - fy);
    double v00 = h01((ix * 0x9E3779B1u) ^ (iy * 0x85EBCA77u) ^ seed);
    double v10 = h01(((ix + 1) * 0x9E3779B1u) ^ (iy * 0x85EBCA77u) ^ seed);
    double v01 = h01((ix * 0x9E3779B1u) ^ ((iy + 1) * 0x85EBCA77u) ^ seed);
    double v11 = h01(((ix + 1) * 0x9E3779B1u) ^ ((iy + 1) * 0x85EBCA77u) ^ seed);
    double a = v00 + (v10 - v00) * tx, b = v01 + (v11 - v01) * tx;
    return a + (b - a) * ty;
}

/* ------------------------------------------------------------------------------------------------ */
/* view state                                                                                       */

typedef struct { double Z, X, Y, sx, sy, hw; u8 obj, cnt; } Row;

static Row      rows[NROWS + 1];
static double   frac;                      /* car progress through its road unit, 0..1 */
static u16      base_pos;                  /* road unit of row 0 */
static double   sm_carx, sm_head;          /* smoothed car_x/8 and view heading (deg*256) */
static bool     sm_valid;
static uint64_t last_ns;
static u16      world_pos;                 /* road unit up to which world_* is integrated */
static u16      stage_end;                 /* road unit at which the stage ends (sim_lookahead finds its 0xFF) */
static double   world_head;                /* road direction, degrees (loops; only used for scenery) */
static double   world_x, world_z;          /* road position in Z units */
static double   view_deg;                  /* camera direction, degrees */
static double   cam_x, cam_z;
static uint64_t emu_next_ns;
static bool     emu_frame;                 /* an emulated 8 Hz frame elapsed this frame */

typedef struct { u16 pos; uint64_t since; bool fade; } SlotFade;
static SlotFade fades[10];

/* Extrapolation between simulation steps. The simulation moves the car 12.5 times a second (sim.c: the
 * 100 Hz driving ISR steps on every eighth tick), so the snapshot the renderer reads only changes that
 * often; drawn straight, the road jumps once per step and stands still in between. Every moving thing is
 * therefore carried on from the last step by the fraction of a step that has elapsed, using the distance
 * it covered over the step before (step_alpha, Track.ds), and the jump is spread over the frames. */
#define SIM_STEP_NS 80000000.0                 /* one 12.5 Hz simulation step */

typedef struct { bool valid; u16 pos; s16 sub; double ds; } Track;   /* ds: units advanced last step */

static Track trk_player, trk_car[10], trk_cop;
static double step_alpha;                  /* progress through the current step, 0..1 */
static uint64_t step_ns;                   /* when the current step was first seen */
static double view_ahead;                  /* the camera's extrapolation, in units, against base_pos + frac
                                            * built from the raw snapshot (negative once a unit was carried) */
static int pos_carry;                      /* whole units the extrapolation is ahead of the simulation */

static void track_reset(void)
{
    trk_player.valid = false;
    trk_cop.valid = false;
    for (int j = 0; j < 10; j++) trk_car[j].valid = false;
    step_alpha = 0;
    step_ns = 0;
}

/* Advance of (pos, sub) over the last step, in road units. sub counts down from 89 within a unit. */
static void track_step(Track *k, u16 pos, s16 sub)
{
    if (!k->valid) {
        *k = (Track){ true, pos, sub, 0.0 };
        return;
    }
    double d = (double)(s16)(u16)(pos - k->pos) + (k->sub - sub) / 90.0;
    k->ds = d < 0 || d > 8 ? 0.0 : d;        /* a reset or a slot reused: no extrapolation */
    k->pos = pos;
    k->sub = sub;
}

/* How far the thing tracked by k has moved since its step, in road units. */
static double track_ahead(const Track *k) { return k->valid ? step_alpha * k->ds : 0.0; }

typedef struct { s16 x0, y0, x1, y1; } Crack;
static Crack cracks[128];
static int   ncracks;

/* Road byte of unit p. End of stage (0xFF) and the original's end-of-road rule read as record 0. */
static u8 road_rec(u16 p, bool *ended)
{
    if (*ended) return 0;
    u8 b = DSB(p);
    if (b == 0xFF) { *ended = true; return 0; }
    if (DSB(DS_g_stageEvent) != 0 && !((s16)p < (s16)DSW(DS_stage_end_pos))) return 0;
    return b < 0x6E ? b : 0;
}

static double sin15(double acc)                /* 15 * sin of an 8.8 fixed degree accumulator */
{
    double deg = fmod(acc / 256.0 + 128.0, 256.0);
    if (deg < 0) deg += 256.0;
    return 15.0 * sin((deg - 128.0) * M_PI / 180.0);
}

static double depth_of(double t) { return 34.0 + 15.0 * t; }   /* original table for t >= 1 */

static void project(Row *r)
{
    double Z = r->Z < 1.0 ? 1.0 : r->Z;
    double sx = 120.0 + 150.0 * r->X / Z;
    r->sx = sx < -4000 ? -4000 : sx > 4000 ? 4000 : sx;
    r->hw = 10125.0 / Z;
    /* PORT: the original divides by sqrt(X^2 + Z^2); plain Z keeps road edges straight near the car */
    double sy = 70.0 - 180.0 * r->Y / Z;
    r->sy = sy < -600 ? -600 : sy > 900 ? 900 : sy;
}

static void update_view(void)
{
    uint64_t now = host_time_ns();
    double dt = last_ns ? (double)(now - last_ns) / 1e9 : 0.0;
    last_ns = now;
    if (dt > 0.25) dt = 0.25;

    double tx = DSS(DS_car_x) / 8.0, th = DSS(DS_view_heading);
    if (!sm_valid) { sm_carx = tx; sm_head = th; sm_valid = true; }
    double k = 1.0 - exp(-dt / 0.045);
    sm_carx += (tx - sm_carx) * k;
    sm_head += (th - sm_head) * k;

    base_pos = DSW(DS_r_road_ptr);
    s16 sub = DSS(DS_r_subpos);
    /* a snapshot that differs from the last one is a new simulation step: note when it arrived and how far
     * everything moved over the step before it */
    if (!trk_player.valid || base_pos != trk_player.pos || sub != trk_player.sub) {
        track_step(&trk_player, base_pos, sub);
        for (int j = 0; j < 10; j++) {
            u16 si = (u16)(DS_r_traffic_slots + 8 * j);
            u16 p = DSW(si);
            if (p == 0) trk_car[j].valid = false;
            else track_step(&trk_car[j], p, DSS((u16)(si + 2)));
        }
        u8 cst = DSB(DS_r_cop_state);
        if (cst == 0 || cst == 7) trk_cop.valid = false;
        else track_step(&trk_cop, DSW(DS_r_cop_pos), DSS(DS_r_cop_sub));
        step_ns = now;
    }
    step_alpha = step_ns && now > step_ns ? (double)(now - step_ns) / SIM_STEP_NS : 0.0;
    if (step_alpha > 1) step_alpha = 1;                    /* a late step: wait for it rather than overshoot */

    u16 raw_pos = base_pos;                                /* the unit the simulation is in */
    frac = (89.0 - sub) / 90.0 + track_ahead(&trk_player);
    view_ahead = track_ahead(&trk_player);
    pos_carry = 0;
    while (frac >= 1.0) {                                  /* the extrapolation crossed into the next unit */
        base_pos++;
        pos_carry++;
        frac -= 1.0;
        view_ahead -= 1.0;
    }
    if (frac < 0) frac = 0;
    if (frac > 0.999) frac = 0.999;

    /* World direction/position along the road, for scenery parallax. Only the simulation's own units are
     * integrated into world_*: a unit the extrapolation crossed is carried in the local copy below, because
     * the next step may still be in the unit before it and world_* must not walk backwards. */
    s16 adv = (s16)(u16)(raw_pos - world_pos);
    if (adv < 0 || adv > 400) world_pos = raw_pos;
    while (world_pos != raw_pos) {
        bool e = false;
        u8 b = road_rec(world_pos, &e);
        world_head += (s8)REC(b, 1) * 64.0 / 256.0;
        double a = world_head * M_PI / 180.0;
        world_x += 15.0 * sin(a);
        world_z += 15.0 * cos(a);
        world_pos++;
    }
    double head = world_head, wx = world_x, wz = world_z;
    for (int i = 0; i < pos_carry; i++) {
        bool e = false;
        u8 b = road_rec((u16)(raw_pos + i), &e);
        head += (s8)REC(b, 1) * 64.0 / 256.0;
        double a = head * M_PI / 180.0;
        wx += 15.0 * sin(a);
        wz += 15.0 * cos(a);
    }
    bool e = false;
    u8 b1 = road_rec(base_pos, &e);
    double c1 = (s8)REC(b1, 1) * 64.0 / 256.0;
    double a = (head + frac * c1) * M_PI / 180.0;
    cam_x = wx + frac * 15.0 * sin(a);
    cam_z = wz + frac * 15.0 * cos(a);
    view_deg = head + frac * c1 - sm_head / 256.0;

    if (now >= emu_next_ns) {
        emu_frame = true;
        emu_next_ns = (emu_next_ns == 0 || now > emu_next_ns + 250000000ull) ? now + 125000000ull
                                                                             : emu_next_ns + 125000000ull;
    } else {
        emu_frame = false;
    }
}

/* Walks the road like 0x2054, but NROWS rows, continuous depth and float trigonometry. The first
 * record's contribution is faded out as the car moves through its unit, so that at frac = 1 row i
 * matches row i-1 of the next unit. */
static void walk_road(void)
{
    bool ended = false, e0 = false;
    /* The counter steps once per road unit (sim.c: sim_advance_unit), so a unit the extrapolation crossed
     * has to be added here; otherwise the dashes, poles and roadside pieces, which are keyed to it, would
     * sit one unit out until the simulation caught up and then snap back. */
    u8 phase = (u8)(DSB(DS_road_anim_counter) + pos_carry);
    u8 b1 = road_rec(base_pos, &e0);
    double head = sm_head - frac * (s8)REC(b1, 1) * 64.0;
    double slope = -frac * (s8)REC(b1, 2) * 4.0;

    /* row 0 lies one unit behind row 1 on the same depth line, below the window; the car (lateral
     * offset sm_carx, height -12) is at depth 0 between them */
    double h1 = head + (s8)REC(b1, 1) * 64.0;
    if (h1 < -0x4B00) h1 = -0x4B00;
    if (h1 > 0x4B00) h1 = 0x4B00;
    double dx1 = 2.0 * sin15(h1), dy1 = sin15(slope + (s8)REC(b1, 2) * 4.0);
    rows[0] = (Row){ .Z = depth_of(-frac), .X = sm_carx - frac * dx1, .Y = -12.0 - frac * dy1 };
    project(&rows[0]);
    for (int i = 1; i <= NROWS; i++) {
        u8 b = road_rec((u16)(base_pos + i - 1), &ended);
        Row *r = &rows[i], *p = &rows[i - 1];
        slope += (s8)REC(b, 2) * 4.0;
        head += (s8)REC(b, 1) * 64.0;
        if (head < -0x4B00) head = -0x4B00;
        if (head > 0x4B00) head = 0x4B00;
        r->X = p->X + 2.0 * sin15(head);
        r->Y = p->Y + sin15(slope);
        r->Z = depth_of(i - frac);
        r->obj = REC(b, 3);
        r->cnt = (u8)(phase + i - 1);
        project(r);
    }
}

/* Interpolated road centre at depth d (row i has depth i - frac). */
static bool sample_road(double d, double *X, double *Y, double *Z)
{
    double u = d + frac;
    if (u < 0 || u >= NROWS) return false;
    int i = (int)u;
    double s = u - i;
    const Row *a = &rows[i], *b = &rows[i + 1];
    *X = a->X + (b->X - a->X) * s;
    *Y = a->Y + (b->Y - a->Y) * s;
    *Z = a->Z + (b->Z - a->Z) * s;
    return true;
}

static double screen_x(double X, double Z) { return 120.0 + 150.0 * X / (Z < 1 ? 1 : Z); }
static double screen_y(double Y, double Z) { return 70.0 - 180.0 * Y / (Z < 1 ? 1 : Z); }

/* ------------------------------------------------------------------------------------------------ */
/* road: drawn near to far; a pixel is written once (zbuf == Z_INF means empty). All passes work on  */
/* a band of internal scanlines [b0, b1) so that bands can run in parallel.                         */

static int ss_col(double x) { return (int)ceil(x * Q - 0.5); }
static int ss_row(double y) { return (int)ceil((y - WIN_Y0) * Q - 0.5); }
static double row_y(int r) { return WIN_Y0 + (r + 0.5) / Q; }

static void clear_rows(int b0, int b1)
{
    for (size_t i = (size_t)b0 * RW; i < (size_t)b1 * RW; i++) zbuf[i] = Z_INF;
    for (int r = b0; r < b1; r++) {
        row_claimed[r] = false;
        row_filled[r] = 0;
        row_suffix[r] = RW;
    }
}

static void put(int r, int x, u32 c, u8 m, float z)
{
    size_t p = (size_t)r * RW + x;
    col[p] = c;
    mat[p] = m;
    zbuf[p] = z;
    row_filled[r]++;
}

static u32 cliff_tab[ZLUT_N];              /* cliff colour per haze step */

static bool row_full(int r) { return row_filled[r] >= RW; }

/* x range swept by an edge pair slanted by `lean` between scanlines r0 and r1 (for culling); the edges
 * may lie up to `slack` px further right */
static bool strip_offscreen(double ax, double ay, double bx, double by, double lean, double slack, int r0, int r1)
{
    double y0 = row_y(r0), y1 = row_y(r1 - 1);
    double v[4] = { ax + lean * (ay - y0), ax + lean * (ay - y1), bx + lean * (by - y0), bx + lean * (by - y1) };
    double lo = v[0], hi = v[0];
    for (int i = 1; i < 4; i++) { if (v[i] < lo) lo = v[i]; if (v[i] > hi) hi = v[i]; }
    return hi + slack < 0 || lo > 320;
}

static void span(int r, double x0, double x1, u32 c, u8 m, float z)
{
    int a = ss_col(x0), b = ss_col(x1);
    if (a < 0) a = 0;
    if (b > RW) b = RW;
    if (a >= b) return;
    int end = b < row_suffix[r] ? b : row_suffix[r];
    const float *pz = zbuf + (size_t)r * RW;
    for (int i = a; i < end; i++)
        if (pz[i] == Z_INF) put(r, i, c, m, z);
    if (b >= row_suffix[r] && a < row_suffix[r]) row_suffix[r] = a;
}

/* Road surface between near row a and far row b (scanlines between their projections). As in the
 * original's scanline fill, everything right of the nearest road row on a scanline is cliff. */
static void road_surface(const Row *a, const Row *b, u8 cnt, u32 cliff, int b0, int b1)
{
    if (a->sy <= b->sy + 1e-9) return;              /* seen from behind (past a crest) */
    int r0 = ss_row(b->sy), r1 = ss_row(a->sy);
    if (r0 < b0) r0 = b0;
    if (r1 > b1) r1 = b1;
    bool alt = (cnt >> 1) & 1, dash = !(cnt & 4);
    for (int r = r0; r < r1; r++) {
        if (row_full(r)) continue;
        double yc = row_y(r);
        double s = (yc - b->sy) / (a->sy - b->sy);
        double cx = b->sx + (a->sx - b->sx) * s, hw = b->hw + (a->hw - b->hw) * s;
        double z = 1.0 / (1.0 / b->Z + (1.0 / a->Z - 1.0 / b->Z) * s);
        float zf = (float)z;
        if (dash) {
            double dw = hw * 0.03;
            if (dw < 0.5 / Q) dw = 0.5 / Q;
            span(r, cx - dw, cx + dw, road_hazed(C_DASH, z), 7, zf);
        }
        u32 road = road_hazed(alt ? C_ROAD_B : C_ROAD_A, z), shld = road_hazed(alt ? C_SHLD_B : C_SHLD_A, z);
        span(r, cx - 1.25 * hw, cx - hw, shld, 1, zf);
        span(r, cx - hw, cx + hw, road, 0, zf);
        span(r, cx + hw, cx + 1.25 * hw, shld, 1, zf);
        if (!row_claimed[r]) {
            row_claimed[r] = true;
            span(r, cx + 1.25 * hw, 1e6, hazed(cliff, z), 2, zf);
        }
    }
}

/* Notch depth `jag` faded in over the first JAG_FOOT px away from the road edge (h px away) */
static double jag_at(double jag, double h)
{
    return h <= 0 ? 0 : h >= JAG_FOOT ? jag : jag * smooth(h / JAG_FOOT);
}

/* The rock face above the right road edge between rows a (near) and b (far): the original's plain
 * colour-2 side, slanted like its cliff-edge sprite, up to the top of the window. The nearest face
 * also covers everything to its right. Every row's edge is notched by the same cliff_jag[] of the
 * scanline (faded in above the road edge), so the outline is uneven like the sprite's and the strips
 * still meet without gaps. */
static void cliff_face(const Row *a, const Row *b, bool nearest, int b0, int b1)
{
    double ax = a->sx + 1.25 * a->hw, bx = b->sx + 1.25 * b->hw;
    double iza = 1.0 / a->Z, izb = 1.0 / b->Z;
    int rmax = ss_row(a->sy > b->sy ? a->sy : b->sy);
    if (rmax > b1) rmax = b1;
    if (rmax <= b0 || (!nearest && strip_offscreen(ax, a->sy, bx, b->sy, CLIFF_LEAN, CLIFF_JAG, b0, rmax))) return;
    for (int r = rmax - 1; r >= b0; r--) {
        double yc = row_y(r);
        double ha = a->sy - yc, hb = b->sy - yc, jag = cliff_jag[r];
        double xa = ax + CLIFF_LEAN * ha + jag_at(jag, ha), xb = bx + CLIFF_LEAN * hb + jag_at(jag, hb);
        if (!nearest && xa > 320 && xb > 320) break;          /* only moves further right upwards */
        if (row_full(r)) continue;
        double dx = xb - xa;
        if (!nearest && fabs(dx) < 1e-9) continue;
        int c0 = ss_col(xa < xb ? xa : xb), c1 = nearest ? RW : ss_col(xa < xb ? xb : xa);
        if (c0 < 0) c0 = 0;
        if (c1 > RW) c1 = RW;
        int end = c1 < row_suffix[r] ? c1 : row_suffix[r];
        if (c0 >= end) continue;
        u32 *pc = col + (size_t)r * RW;
        u8 *pm = mat + (size_t)r * RW;
        float *pz = zbuf + (size_t)r * RW;
        bool all = true, low = yc > (a->sy < b->sy ? a->sy : b->sy);   /* the foot test can fail */
        double dt = fabs(dx) < 1e-9 ? 0.0 : 1.0 / (dx * Q);
        double t0 = (c0 + 0.5 - xa * Q) * dt;
        int filled = 0;
        for (int c = c0; c < end; c++) {
            if (pz[c] != Z_INF) continue;
            double t = t0 + (c - c0) * dt;
            t = t < 0 ? 0 : t > 1 ? 1 : t;
            if (low && yc > a->sy + (b->sy - a->sy) * t) { all = false; continue; }   /* below the road edge */
            double z = 1.0 / (iza + (izb - iza) * t);
            pc[c] = cliff_tab[zlut_idx(z)];
            pm[c] = 2;
            pz[c] = (float)z;
            filled++;
        }
        row_filled[r] += filled;
        if (all && c1 >= row_suffix[r]) row_suffix[r] = c0;
    }
}

/* Below the left road edge: a dark rim straight down, then the hillside falling away outwards to the
 * bottom of the window. On a straight road the hillside stays under the road; on left bends it
 * carries the far road. The hillside's outer edge is notched like the rock face (hill_jag[]). */
static void left_side(const Row *a, const Row *b, int b0, int b1)
{
    double ax = a->sx - 1.25 * a->hw, bx = b->sx - 1.25 * b->hw;
    double iza = 1.0 / a->Z, izb = 1.0 / b->Z;
    int r0 = ss_row(a->sy < b->sy ? a->sy : b->sy);
    if (r0 < b0) r0 = b0;
    if (r0 >= b1 || strip_offscreen(ax, a->sy, bx, b->sy, -HILL_LEAN, CLIFF_JAG, r0, b1)) return;
    double zmin = a->Z < b->Z ? a->Z : b->Z;
    int rim_end = ss_row((a->sy > b->sy ? a->sy : b->sy) + 180.0 * RIM_H / zmin) + 1;
    for (int r = r0; r < b1; r++) {
        double yc = row_y(r);
        double ha = yc - a->sy, hb = yc - b->sy, jag = hill_jag[r];
        double xa = ax - HILL_LEAN * ha, xb = bx - HILL_LEAN * hb;
        bool rim = r < rim_end && fabs(bx - ax) > 1e-9;
        if (!rim && xa + CLIFF_JAG < 0 && xb + CLIFF_JAG < 0) break;   /* only moves further left downwards */
        xa += jag_at(jag, ha);
        xb += jag_at(jag, hb);
        if (row_full(r)) continue;
        const float *pz = zbuf + (size_t)r * RW;
        /* rim: vertical, between the unslanted edge points */
        double dx = bx - ax;
        if (rim) {
            int c0 = ss_col(ax < bx ? ax : bx), c1 = ss_col(ax < bx ? bx : ax);
            if (c0 < 0) c0 = 0;
            if (c1 > RW) c1 = RW;
            for (int c = c0; c < c1; c++) {
                if (pz[c] != Z_INF) continue;
                double t = ((c + 0.5) / Q - ax) / dx;
                double foot = a->sy + (b->sy - a->sy) * t;
                double z = 1.0 / (iza + (izb - iza) * t);
                double v = (yc - foot) * z / 180.0;
                if (v < 0 || v > RIM_H) continue;
                put(r, c, hazed(C_RIM, z), 2, (float)z);
            }
        }
        /* hillside: slanted outwards, starting below the rim */
        dx = xb - xa;
        if (fabs(dx) < 1e-9) continue;
        int c0 = ss_col(xa < xb ? xa : xb), c1 = ss_col(xa < xb ? xb : xa);
        if (c0 < 0) c0 = 0;
        if (c1 > RW) c1 = RW;
        for (int c = c0; c < c1; c++) {
            if (pz[c] != Z_INF) continue;
            double t = ((c + 0.5) / Q - xa) / dx;
            t = t < 0 ? 0 : t > 1 ? 1 : t;
            double foot = a->sy + (b->sy - a->sy) * t;
            if (yc < foot) continue;
            double z = 1.0 / (iza + (izb - iza) * t);
            double v = (yc - foot) * z / 180.0;
            u32 c2 = v < RIM_H ? C_RIM : blend(C_HILL_TOP, C_HILL, (float)(v > 60.0 ? 1.0 : v / 60.0));
            put(r, c, hazed(c2, z), 2, (float)z);
        }
    }
}

static u32 cliff_colour;

static void prepare_road(void)
{
    cliff_colour = gfx_palette_rgb(10);             /* colour 2 of the road buffer on screen */
    for (int i = 0; i < ZLUT_N; i++) cliff_tab[i] = blend(cliff_colour, C_HAZE, haze_lut[i]);
    /* Notches of the rock face's and the hillside's edges: small bumps with slight roughness in them,
     * fixed to the screen like a distant outline. */
    for (int r = 0; r < RH; r++) {
        double y = row_y(r);
        long n = 1L << 20;
        cliff_jag[r] = CLIFF_JAG * (0.8 * noise1(y / 5.0, n, 0xC11FF) + 0.2 * noise1(y / 2.0, n, 0x5CA12));
        hill_jag[r] = CLIFF_JAG * (0.85 * noise1(y / 12.0, n, 0x1B0A7) + 0.15 * noise1(y / 5.0, n, 0x7E3D5));
    }
}

static void draw_road(int b0, int b1)
{
    for (int i = 0; i < NROWS; i++) {
        const Row *a = &rows[i], *b = &rows[i + 1];
        road_surface(a, b, b->cnt, cliff_colour, b0, b1);
        cliff_face(a, b, i == 0, b0, b1);
        left_side(a, b, b0, b1);
    }
}

/* ------------------------------------------------------------------------------------------------ */
/* background where nothing was drawn: sky, mountains, valley                                       */

static double bg_sv, bg_cv;                 /* view direction for the valley floor */
static float *mist;                         /* RH: ground-mist weight above the horizon */

#define HORIZON_HAZE 0.6f                   /* haze where the valley floor meets the mountains */
#define MIST_ROWS    5.0                    /* mist falloff above the horizon, in game rows */

static void prepare_background(void)
{
    double va = view_deg * M_PI / 180.0;
    bg_sv = sin(va);
    bg_cv = cos(va);

    /* mountain heights per internal column */
    for (int c = 0; c < RW; c++) {
        double xc = (c + 0.5) / Q;
        double th = view_deg + atan((xc - 120.0) / 150.0) * 180.0 / M_PI;
        double u = th / 360.0;
        double big = noise1(u * 26.0, 26, 101);
        double hf = 3.0 + 12.0 * big * big + 3.0 * noise1(u * 110.0, 110, 102) + noise1(u * 420.0, 420, 103);
        double hn = 16.0 * (noise1(u * 14.0, 14, 201) - 0.45) + 2.5 * noise1(u * 90.0, 90, 202)
                    + 0.8 * noise1(u * 360.0, 360, 203);
        far_top[c] = (float)(HORIZON - hf);
        near_top[c] = (float)(HORIZON - (hn > 0 ? hn : 0));
        snow[c] = hf > 12.5 ? (float)(HORIZON - hf + (hf - 12.5) * 0.45) : -1e9f;   /* snow line */
    }
    for (int r = 0; r < RH; r++) {
        float t = (float)((row_y(r) - WIN_Y0) / (HORIZON - WIN_Y0));
        sky[r] = pack(mix(hex(C_SKY_TOP), hex(C_SKY_LOW), t > 1 ? 1 : t * t * (2.0f - t)));
        double h = HORIZON - row_y(r);      /* rows above the horizon: ground mist thins out over a few rows */
        mist[r] = h > 0 ? (float)exp(-h / MIST_ROWS) : 1.0f;
    }
}

/* Valley floor for output row oy (output resolution). */
static void valley_row(int oy)
{
    u32 *dst = valley + (size_t)oy * OW;
    double y = WIN_Y0 + (oy + 0.5) / OK;
    double dy = y - HORIZON;
    if (dy <= 0) return;
    double D = 180.0 * VALLEY_H / dy;
    /* approaches HORIZON_HAZE at the horizon, the same amount the mountain feet get, so there is no seam */
    float hz = (float)(HORIZON_HAZE * (1.0 - exp(-D / 45000.0)));
    double contrast = exp(-D / 30000.0);
    Rgb fa = hex(C_FIELD_A), fb = hex(C_FIELD_B), fc = hex(C_FIELD_C), wood = hex(C_WOOD), hazec = hex(C_HAZE);
    for (int x = 0; x < OW; x++) {
        double lat = ((x + 0.5) / OK - 120.0) * D / 150.0;
        double wx = cam_x + D * bg_sv + lat * bg_cv, wz = cam_z + D * bg_cv - lat * bg_sv;
        double n = 0.6 * noise2(wx / 2600.0, wz / 2600.0, 11) + 0.4 * noise2(wx / 800.0, wz / 800.0, 23);
        n = 0.5 + (n - 0.5) * contrast;
        Rgb c = n < 0.5 ? mix(fa, fb, (float)(n * 2.0)) : mix(fb, fc, (float)((n - 0.5) * 2.0));
        double w = (noise2(wx / 420.0, wz / 420.0, 37) - 0.62) * 3.0 * contrast;
        if (w > 0) c = mix(c, wood, (float)(w > 1 ? 1 : w));
        dst[x] = pack(mix(c, hazec, hz));
    }
}

static void draw_background(int b0, int b1)
{
    for (int oy = b0 / SSF; oy < b1 / SSF; oy++) valley_row(oy);
    u32 mf = blend(C_MTN_FAR, C_HAZE, 0.2f), mn = C_MTN_NEAR, sn = 0xEAF1F6;
    for (int r = b0; r < b1; r++) {
        if (row_full(r)) continue;
        double yc = row_y(r);
        u32 *pc = col + (size_t)r * RW;
        u8 *pm = mat + (size_t)r * RW;
        const float *pz = zbuf + (size_t)r * RW;
        int oy = r / SSF;
        const u32 *vr = WIN_Y0 + (oy + 0.5) / OK > HORIZON ? valley + (size_t)oy * OW : NULL;
        for (int c = 0; c < RW; c++) {
            if (pz[c] != Z_INF) continue;
            pm[c] = 4;                              /* the original's open side is colour 4 */
            if (vr) {
                pc[c] = vr[c / SSF];
            } else if (yc >= near_top[c]) {
                pc[c] = blend(mn, C_HAZE, 0.05f + (HORIZON_HAZE - 0.05f) * mist[r]);
            } else if (yc >= far_top[c]) {
                /* mf already carries 0.2 haze: 1 - 0.8 * (1 - 0.5) = HORIZON_HAZE at the foot */
                pc[c] = blend(yc < snow[c] ? sn : mf, C_HAZE, 0.05f + 0.45f * mist[r]);
            } else {
                pc[c] = sky[r];
            }
        }
    }
}

/* ------------------------------------------------------------------------------------------------ */
/* sprites                                                                                          */

#define SPR_MIPS 6

typedef struct {
    u16 seg, off;
    int w, h, hx, hy, ox, oy;
    u8 pm[4], nst;
    u8 *bits;                              /* per pixel: bit k = stored plane k */
    /* reduced copies for strong downscaling: level L (1..nmip) has texels of 2^L x 2^L source pixels, each
     * the most frequent pattern that changes something under op and the share of such pixels (0..255) */
    int nmip;
    int mip_w[SPR_MIPS + 1], mip_h[SPR_MIPS + 1], mip_off[SPR_MIPS + 1];
    u8 *mip[4];                            /* [op]: pattern, coverage pairs of all levels */
} Spr;

#define SPR_CACHE 400
static Spr cache[SPR_CACHE];
static int ncache;

static void cache_clear(void)
{
    for (int i = 0; i < ncache; i++) {
        free(cache[i].bits);
        for (int op = 0; op < 4; op++) free(cache[i].mip[op]);
    }
    ncache = 0;
}

static void build_mips(Spr *s);

static const Spr *spr_get(FarPtr p)
{
    for (int i = 0; i < ncache; i++)
        if (cache[i].seg == p.seg && cache[i].off == p.off) return &cache[i];
    if ((p.seg == 0 && p.off == 0) || ncache == SPR_CACHE) return NULL;
    u16 wb = rd16(p.seg, p.off), h = rd16(p.seg, (u16)(p.off + 2));
    if (wb == 0 || h == 0 || wb > 40 || h > 200) return NULL;
    Spr *s = &cache[ncache];
    s->seg = p.seg; s->off = p.off;
    s->w = wb * 8; s->h = h;
    s->hx = (s16)rd16(p.seg, (u16)(p.off + 4));
    s->hy = (s16)rd16(p.seg, (u16)(p.off + 6));
    s->ox = (s16)rd16(p.seg, (u16)(p.off + 8)) & ~3;
    s->oy = (s16)rd16(p.seg, (u16)(p.off + 10));
    for (int k = 0; k < 4; k++) s->pm[k] = rd8(p.seg, (u16)(p.off + 12 + k));
    s->nst = 0;
    while (s->nst < 4 && (s->pm[s->nst] & 0x0F)) s->nst++;
    u16 block = (u16)(h * wb + (s->pm[3] >> 4));
    s->bits = calloc((size_t)s->w * h, 1);
    s->nmip = 0;
    for (int op = 0; op < 4; op++) s->mip[op] = NULL;
    if (!s->bits) return NULL;
    for (int k = 0; k < s->nst; k++) {
        u16 src = (u16)(p.off + 0x10 + k * block);
        for (int y = 0; y < h; y++)
            for (int x = 0; x < s->w; x++)
                if (rd8(p.seg, (u16)(src + y * wb + (x >> 3))) & (0x80 >> (x & 7)))
                    s->bits[y * s->w + x] |= (u8)(1 << k);
    }
    build_mips(s);
    ncache++;
    return s;
}

typedef struct { u8 a, o, x, touch; } Lut;

/* Per stored-bit-pattern effect on a 3-plane colour index: new = ((old & a) | o) ^ x. Mirrors the
 * blitters' plane map handling (clear/set nibbles, data planes) for the road buffer's planes 0..2. */
static void make_lut(const Spr *s, int op, Lut lut[16])
{
    u8 clr = (s->pm[0] >> 4) & 7, set = (s->pm[1] >> 4) & 7, has = 0;
    for (int k = 0; k < s->nst; k++) has |= s->pm[k] & 7;
    for (int v = 0; v < 16; v++) {
        u8 d_or = 0, d_and = 7, d_xor = 0, d_first = 0, got = 0;
        for (int k = 0; k < s->nst; k++) {
            u8 planes = s->pm[k] & 7;
            bool bit = (v >> k) & 1;
            if (bit) { d_or |= planes; d_xor ^= planes; }
            else d_and &= (u8)~planes;
            d_first |= (u8)((bit ? planes : 0) & ~got);
            got |= planes;
        }
        Lut L = { 7, 0, 0, 0 };
        switch (op) {
        case OP_AND:  L.a = (u8)(d_and & ~clr & 7); break;
        case OP_OR:   L.o = (u8)(d_or | set); break;
        case OP_XOR:  L.x = (u8)(d_xor ^ set); break;
        default:      L.a = (u8)(~(clr | set | has) & 7); L.o = (u8)((set & ~has) | d_first); break;
        }
        L.touch = L.a != 7 || L.o || L.x;
        lut[v] = L;
    }
}

/* Reduced copies of a sprite for each operation (the samples hold colour indices for the operations that
 * follow, so texels cannot be averaged): a texel of level L covers 2^L x 2^L source pixels and holds the
 * most frequent of their patterns that change something under the operation, and the share of such pixels.
 * Drawn with that share as a dithered coverage, a sprite scaled far down keeps its average shape and colour
 * from frame to frame instead of sparkling between the source pixels a sample happens to hit. */
static void build_mips(Spr *s)
{
    int n = 0, off = 0;
    for (int L = 1; L <= SPR_MIPS; L++) {
        int w = (s->w + (1 << L) - 1) >> L, h = (s->h + (1 << L) - 1) >> L;
        s->mip_w[L] = w;
        s->mip_h[L] = h;
        s->mip_off[L] = off;
        off += w * h;
        n = L;
        if (w <= 1 && h <= 1) break;
    }
    s->nmip = n;
    for (int op = 0; op < 4; op++) {
        u8 *m = malloc((size_t)off * 2);
        s->mip[op] = m;
        if (!m) { s->nmip = 0; continue; }
        Lut lut[16];
        make_lut(s, op, lut);
        for (int L = 1; L <= n; L++) {
            int bs = 1 << L;
            for (int ty = 0; ty < s->mip_h[L]; ty++)
                for (int tx = 0; tx < s->mip_w[L]; tx++) {
                    int cnt[16] = { 0 }, area = 0, hit = 0;
                    for (int y = ty * bs; y < ty * bs + bs && y < s->h; y++)
                        for (int x = tx * bs; x < tx * bs + bs && x < s->w; x++) {
                            u8 v = s->bits[y * s->w + x];
                            area++;
                            if (lut[v].touch) { cnt[v]++; hit++; }
                        }
                    int best = 0;
                    for (int v = 1; v < 16; v++) if (cnt[v] > cnt[best]) best = v;
                    u8 *t = m + 2 * (s->mip_off[L] + ty * s->mip_w[L] + tx);
                    t[0] = (u8)(hit ? best : 0);
                    t[1] = (u8)(area ? (hit * 255 + area / 2) / area : 0);
                }
        }
    }
}

static const u8 BAYER[4][4] = { { 0, 8, 2, 10 }, { 12, 4, 14, 6 }, { 3, 11, 1, 9 }, { 15, 7, 13, 5 } };

static bool dither_pass(float cov, int c, int r) { return (float)BAYER[r & 3][c & 3] + 0.5f < cov * 16.0f; }

typedef struct {
    const Spr *s;
    int op, prio, sub, seq;
    bool raw;                              /* position is the top-left corner (no hotspot) */
    double x, y, k, z;
    float alpha;
    int base;                              /* XOR pieces: colour index they are applied to (-1: the pixel's) */
    Lut lut[16];
    u32 pal[8];
} Item;

#define MAX_ITEMS 768
static Item items[MAX_ITEMS];
static int nitems;

static Item *add_item(u16 handle_addr, int op, int prio, int sub, bool raw, double x, double y, double k,
                      double z, float alpha)
{
    if (nitems == MAX_ITEMS) return NULL;
    const Spr *spr = spr_get(far_rd(DGROUP, handle_addr));   /* decoded here, not on the worker threads */
    if (!spr) return NULL;
    Item *it = &items[nitems];
    *it = (Item){ spr, op, prio, sub, nitems, raw, x, y, k, z, alpha, -1, { { 0 } }, { 0 } };
    make_lut(spr, op, it->lut);
    for (int m = 0; m < 8; m++) it->pal[m] = hazed(gfx_palette_rgb((u8)(m | 8)), z);
    nitems++;
    return it;
}

static int item_cmp(const void *pa, const void *pb)
{
    const Item *a = pa, *b = pb;
    if (a->prio != b->prio) return a->prio - b->prio;
    if (a->z != b->z) return a->z > b->z ? -1 : 1;          /* far first */
    if (a->sub != b->sub) return a->sub - b->sub;
    return a->seq - b->seq;
}

static void draw_item(const Item *it, int b0, int b1)
{
    const Spr *s = it->s;
    double k = it->k;
    if (s->h * k * Q < 0.75) return;
    double x0 = it->x - (it->raw ? 0 : s->hx * k), y0 = it->y - (it->raw ? 0 : s->hy * k);
    int c0 = ss_col(x0), c1 = ss_col(x0 + s->w * k), r0 = ss_row(y0), r1 = ss_row(y0 + s->h * k);
    if (c0 < 0) c0 = 0;
    if (c1 > RW) c1 = RW;
    if (r0 < b0) r0 = b0;
    if (r1 > b1) r1 = b1;
    if (c0 >= c1 || r0 >= r1) return;

    float zt = (float)it->z - Z_EPS;
    double inv = 1.0 / (k * Q);
    /* source pixels per output pixel: from two on, the reduced copy whose texels are about that size */
    int L = 0;
    for (double px = 1.0 / (k * OK); L < s->nmip && px >= (double)(2 << L); ) L++;
    if (L > 0 && s->mip[it->op]) {
        const u8 *m = s->mip[it->op] + 2 * s->mip_off[L];
        int mw = s->mip_w[L], mh = s->mip_h[L];
        for (int r = r0; r < r1; r++) {
            int sy = (int)floor((r + 0.5 + (WIN_Y0 - y0) * Q) * inv);
            sy = (sy < 0 ? 0 : sy >= s->h ? s->h - 1 : sy) >> L;
            const u8 *mrow = m + 2 * (size_t)(sy < mh ? sy : mh - 1) * mw;
            size_t row = (size_t)r * RW;
            for (int c = c0; c < c1; c++) {
                size_t p = row + c;
                if (zbuf[p] < zt) continue;
                int sx = (int)floor((c + 0.5 - x0 * Q) * inv);
                sx = (sx < 0 ? 0 : sx >= s->w ? s->w - 1 : sx) >> L;
                const u8 *e = mrow + 2 * (sx < mw ? sx : mw - 1);
                if (!e[1] || (e[1] < 255 && !dither_pass(e[1] * (1.0f / 255.0f), c, r))) continue;
                const Lut *T = &it->lut[e[0]];
                if (it->base >= 0 && mat[p] == 4) continue;
                u8 m2 = (u8)((((it->base < 0 ? mat[p] : (u8)it->base) & T->a) | T->o) ^ T->x);
                mat[p] = m2;
                col[p] = it->alpha >= 1.0f ? it->pal[m2] : blend(col[p], it->pal[m2], it->alpha);
            }
        }
        return;
    }
    for (int r = r0; r < r1; r++) {
        int sy = (int)floor((r + 0.5 + (WIN_Y0 - y0) * Q) * inv);
        if (sy < 0) sy = 0;
        if (sy >= s->h) sy = s->h - 1;
        const u8 *srow = s->bits + sy * s->w;
        size_t row = (size_t)r * RW;
        for (int c = c0; c < c1; c++) {
            size_t p = row + c;
            if (zbuf[p] < zt) continue;
            int sx = (int)floor((c + 0.5 - x0 * Q) * inv);
            if (sx < 0) sx = 0;
            if (sx >= s->w) sx = s->w - 1;
            const Lut *L = &it->lut[srow[sx]];
            if (!L->touch) continue;
            if (it->base >= 0 && mat[p] == 4) continue;        /* cliff-foot pieces stay off the sky and valley */
            u8 m = (u8)((((it->base < 0 ? mat[p] : (u8)it->base) & L->a) | L->o) ^ L->x);
            mat[p] = m;
            col[p] = it->alpha >= 1.0f ? it->pal[m] : blend(col[p], it->pal[m], it->alpha);
        }
    }
}

/* Size classes (W = quarter-pixel road half-width at the object). Poles, posts, signs and roadside
 * pieces come in 4 scales drawn for W = 128 * (idx + 1); traffic comes in 5 scales (table offsets
 * 0, 8, .. 32) drawn for the half-widths in TRAFFIC_W. The original picks the scale from the row. With
 * --sprite-detail max every object uses its largest, most detailed scale at every distance, scaled down
 * from the reduced copies (build_mips). With auto the most detailed scale is used that is still drawn at
 * LOD_MIN_K of its size or larger, so the smaller sprites give way to the larger ones further away, and
 * sprites are mostly scaled down; traffic never uses its smallest scale (offset 0). */
#define LOD_MIN_K 0.5

bool enh_sprite_detail_max = true;

static int small_idx(double W)
{
    if (enh_sprite_detail_max) return 3;
    for (int idx = 3; idx > 0; idx--)
        if (128.0 * (idx + 1) * LOD_MIN_K <= W) return idx;
    return 0;
}
static double small_k(double W, int idx)
{
    double k = W / (128.0 * (idx + 1));
    return k > 1.6 ? 1.6 : k;
}
static const double TRAFFIC_W[5] = { 65, 129, 198, 300, 461 };
#define TRAFFIC_TOP 32                         /* table offset of the largest traffic scale */

static double traffic_k(double Z, u8 ts)
{
    double k = (10125.0 * 4.0 / Z) / TRAFFIC_W[ts / 8];
    return k > 1.35 ? 1.35 : k;
}

static u8 traffic_scale(double Z)
{
    if (enh_sprite_detail_max) return TRAFFIC_TOP;
    double W = 10125.0 * 4.0 / Z;
    for (int l = 4; l > 1; l--)
        if (TRAFFIC_W[l] * LOD_MIN_K <= W) return (u8)(l * 8);
    return 8;                                  /* the smallest scale's heavy outline stands out too much */
}

static float fade_alpha(int j, u16 pos, double t, uint64_t now)
{
    SlotFade *f = &fades[j];
    if (f->pos == 0 || abs((s16)(u16)(pos - f->pos)) > 4) { f->since = now; f->fade = t > 25.0; }
    f->pos = pos;
    if (!f->fade) return 1.0f;
    double a = (double)(now - f->since) / 0.35e9;
    return a >= 1.0 ? 1.0f : (float)a;
}

static void collect_objects(void)
{
    nitems = 0;
    uint64_t now = host_time_ns();

    /* rows: poles, signs, roadside pieces */
    for (int i = 1; i < NROWS; i++) {
        const Row *r = &rows[i];
        double W = r->hw * 4.0;
        int idx = small_idx(W);
        double k = small_k(W, idx);
        if (!(r->cnt & 0x0F)) {                                  /* pole every 16 units, left shoulder */
            u16 e = (u16)(0x0FA7 + 4 * idx);
            const Spr *pol = spr_get(far_rd(DGROUP, (u16)(e + 0x10)));
            double top = r->sy - (pol ? pol->h : 8) * k;
            add_item(e, OP_AND, 1, 6, false, r->sx - 1.25 * r->hw, top, k, r->Z, 1.0f);
            add_item((u16)(e + 0x10), OP_OR, 1, 7, false, r->sx - 1.25 * r->hw, top, k, r->Z, 1.0f);
        }
        u8 t = (u8)((r->obj & 0x3F) - 2);
        if (t <= 6) {                                            /* signs with post */
            double x = (r->obj & 0x80) ? r->sx - r->hw : r->sx + r->hw;
            u16 post = (u16)(0x1123 + 4 * idx);
            u16 img = (u16)(0x1043 + (t << 5) + 8 * idx);
            const Spr *ps = spr_get(far_rd(DGROUP, post));
            double top = r->sy - (ps ? ps->h : 8) * k;
            add_item((u16)(img + 4), OP_AND, 1, 4, false, x, top, k, r->Z, 1.0f);
            add_item(img, OP_OR, 1, 4, false, x, top, k, r->Z, 1.0f);
            add_item(post, OP_OR, 1, 5, true, x - (ps ? ps->hx : 0) * k, top, k, r->Z, 1.0f);
        }
        if (i < 40) {                                            /* cliff-foot pieces, XOR as the original */
            u16 pe = DSW((u16)(DS_roadside_pattern + (((u8)(r->cnt << 1)) & 0x1E)));
            u8 type = (u8)pe, cnt = (u8)(pe >> 8);
            if (type < 6) {
                double s = W / 16.0 > 31 ? 31 : W / 16.0;
                double y = r->sy - floor(s / 2.0) * cnt;
                Item *it = add_item((u16)(0x0FC7 + (type << 4) + 4 * idx), OP_XOR, 0, 0, false,
                                    r->sx + 1.25 * r->hw, y, k, r->Z, 1.0f);
                /* The original XORs them onto whatever lies below, so the grass mounds (rck*, types 0-1)
                 * straddling the shoulder edge came out half green, half black. Apply each type to one
                 * backdrop: mounds to the shoulder (green), tufts and cracks to the cliff. */
                if (it) it->base = type < 2 ? 1 : 2;
            }
        }
    }

    /* traffic */
    s16 subpos = DSS(DS_r_subpos);
    for (int j = 0; j < 10; j++) {
        u16 si = (u16)(DS_r_traffic_slots + 8 * j);
        u16 pos = DSW(si);
        if (pos == 0) { fades[j].pos = 0; continue; }
        double t = (s16)(u16)(pos - base_pos) + (subpos - DSS((u16)(si + 2))) / 90.0
                   + track_ahead(&trk_car[j]) - view_ahead;      /* both carried on from the last step */
        double X, Y, Z;
        if (t < 0.4 || !sample_road(t, &X, &Y, &Z)) continue;
        double Xl = X + (j < 5 ? -ROAD_HW / 2 : ROAD_HW / 2);
        u8 ts = traffic_scale(Z);
        u16 e = (u16)(ts + DSW((u16)(si + 6)));
        double k = traffic_k(Z, ts);
        float alpha = fade_alpha(j, pos, t, now);
        double x = screen_x(Xl, Z), y = screen_y(Y, Z);
        add_item((u16)(0x1177 + e), OP_AND, 1, 3, false, x, y, k, Z, alpha);
        add_item((u16)(0x1173 + e), OP_OR, 1, 3, false, x, y, k, Z, alpha);
    }

    /* police car ahead */
    u8 cs = DSB(DS_r_cop_state);
    if (cs != 0 && cs != 7) {
        /* cop_sub counts down within the unit like the player's sub_unit (sim.c: cop_adv), so the
         * sub-unit term has the same sign as the traffic cars' above */
        double t = (s16)(u16)(DSW(DS_r_cop_pos) - base_pos) + (subpos - DSS(DS_r_cop_sub)) / 90.0
                   + track_ahead(&trk_cop) - view_ahead;
        double X, Y, Z;
        if (t >= 0.4 && sample_road(t, &X, &Y, &Z)) {
            double Xc = X + ROAD_HW / 2 - DSS(DS_r_cop_lane) * ROAD_HW / 16.0;
            u8 ts = traffic_scale(Z);
            u16 e = (u16)(0x132B + ts);
            double k = traffic_k(Z, ts);
            double x = screen_x(Xc, Z), y = screen_y(Y, Z);
            add_item((u16)(e + 4), OP_AND, 1, 0, false, x, y, k, Z, 1.0f);
            add_item(e, OP_OR, 1, 0, false, x, y, k, Z, 1.0f);
            if (!(DSB(DS_g_stageTime) & 8)) {
                add_item((u16)(e + 0x2C), OP_AND, 1, 1, false, x, y, k, Z, 1.0f);
                add_item((u16)(e + 0x28), OP_OR, 1, 1, false, x, y, k, Z, 1.0f);
            }
            if (ts == TRAFFIC_TOP) {           /* the original adds it to the largest scale (always, with max) */
                add_item(0x137F, OP_AND, 1, 2, false, x, y, k, Z, 1.0f);
                add_item(0x137B, OP_OR, 1, 2, false, x, y, k, Z, 1.0f);
            }
        }
    }

    /* hazard (slot 12) */
    u16 hp = DSW(DS_r_hazard_slot);
    s16 hr = (s16)(u16)(hp - base_pos);
    if (hp != 0 && hr >= 1 && hr < NROWS) {
        double X, Y, Z;
        if (sample_road(hr - frac, &X, &Y, &Z)) {
            u16 w6 = DSW((u16)(DS_r_hazard_slot + 6));
            double Xh = X + (ROAD_HW / 4.0) * (0.5 + (w6 >> 8));       /* lane quarters right of centre */
            double W = 10125.0 * 4.0 / Z;
            int idx = small_idx(W);
            add_item((u16)(0x1133 + 4 * idx + (w6 & 0xFF)), OP_OR, 0, 0, false,
                     screen_x(Xh, Z), screen_y(Y, Z), small_k(W, idx), Z, 1.0f);
        }
    }
}

static void sort_objects(void) { qsort(items, (size_t)nitems, sizeof items[0], item_cmp); }

static void draw_objects(int b0, int b1)
{
    for (int i = 0; i < nitems; i++) draw_item(&items[i], b0, b1);
}

/* ------------------------------------------------------------------------------------------------ */
/* output, coverage, overlay                                                                        */

static float sharpen = 0.4f;               /* unsharp mask amount after the resolve */

/* Output rows [o0, o1): average SSF x SSF samples in linear light. */
static void resolve_rows(int o0, int o1)
{
    u32 n = (u32)(SSF * SSF), h = n / 2;
    for (int y = o0; y < o1; y++) {
        u32 *dst = soft + (size_t)y * OW;
        for (int x = 0; x < OW; x++) {
            u32 r = 0, g = 0, b = 0;
            for (int j = 0; j < SSF; j++) {
                const u32 *p = col + (size_t)(y * SSF + j) * RW + (size_t)x * SSF;
                for (int i = 0; i < SSF; i++) {
                    r += to_linear[p[i] >> 16 & 255];
                    g += to_linear[p[i] >> 8 & 255];
                    b += to_linear[p[i] & 255];
                }
            }
            dst[x] = (u32)to_srgb[(r + h) / n] << 16 | (u32)to_srgb[(g + h) / n] << 8 | to_srgb[(b + h) / n];
        }
    }
}

/* Output rows [o0, o1): unsharp mask from the resolved image (reads the neighbouring rows). */
static void sharpen_rows(int o0, int o1)
{
    int amt = (int)(sharpen * 256.0f);
    for (int y = o0; y < o1; y++) {
        const u32 *up = soft + (size_t)(y > 0 ? y - 1 : y) * OW, *cur = soft + (size_t)y * OW;
        const u32 *dn = soft + (size_t)(y < OH - 1 ? y + 1 : y) * OW;
        u32 *dst = out + (size_t)y * OW;
        for (int x = 0; x < OW; x++) {
            int xl = x > 0 ? x - 1 : x, xr = x < OW - 1 ? x + 1 : x;
            u32 v = 0;
            for (int sh = 0; sh <= 16; sh += 8) {
                int c = (int)(cur[x] >> sh & 255);
                int nb = (int)(up[x] >> sh & 255) + (int)(dn[x] >> sh & 255)
                         + (int)(cur[xl] >> sh & 255) + (int)(cur[xr] >> sh & 255);
                int o = c + ((c * 4 - nb) * amt >> 10);
                v |= (u32)(o < 0 ? 0 : o > 255 ? 255 : o) << sh;
            }
            dst[x] = v;
        }
    }
}

/* ------------------------------------------------------------------------------------------------ */
/* frame orchestration                                                                              */

/* TD_ENH_STATS=1: render and overlay times on stderr every STATS_EVERY frames, with the rate the frames
 * were actually produced at (the stage loop asks for --frame-rate, but a frame that takes longer than its
 * period pushes the next one back). */
#define STATS_EVERY 300

static bool stats_on;
static int stats_frames;
static double stats_render_ms, stats_render_max, stats_overlay_ms;
static uint64_t stats_since_ns;

static void stats_add(double *acc, uint64_t t0)
{
    if (stats_on) *acc += (double)(host_time_ns() - t0) / 1e6;
}

static void stats_report(void)
{
    uint64_t now = host_time_ns();
    if (!stats_since_ns) stats_since_ns = now;
    if (++stats_frames < STATS_EVERY) return;
    double secs = (double)(now - stats_since_ns) / 1e9;
    fprintf(stderr, "enh: render %.1f ms avg, %.1f ms max; overlay %.1f ms avg; %d frames in %.1f s = %.1f fps\n",
            stats_render_ms / stats_frames, stats_render_max, stats_overlay_ms / stats_frames, stats_frames,
            secs, secs > 0 ? stats_frames / secs : 0.0);
    stats_frames = 0;
    stats_render_ms = stats_render_max = stats_overlay_ms = 0;
    stats_since_ns = now;
}

static void *xrealloc(void *p, size_t n)
{
    void *q = realloc(p, n ? n : 1);
    if (!q) { free(p); return NULL; }
    return q;
}

static bool ensure_buffers(void)
{
    int k = gfx_output_scale();
    if (k == OK && col) return true;
    OK = k;
    SSF = k == 1 ? 4 : 2;
    Q = OK * SSF;
    OW = 320 * OK;
    OH = WIN_H * OK;
    RW = 320 * Q;
    RH = WIN_H * Q;
    sharpen = k == 1 ? 0.4f : 0.25f;
    size_t rp = (size_t)RW * RH, op = (size_t)OW * OH;
    col = xrealloc(col, rp * sizeof *col);
    mat = xrealloc(mat, rp * sizeof *mat);
    zbuf = xrealloc(zbuf, rp * sizeof *zbuf);
    row_claimed = xrealloc(row_claimed, (size_t)RH * sizeof *row_claimed);
    row_filled = xrealloc(row_filled, (size_t)RH * sizeof *row_filled);
    row_suffix = xrealloc(row_suffix, (size_t)RH * sizeof *row_suffix);
    sky = xrealloc(sky, (size_t)RH * sizeof *sky);
    cliff_jag = xrealloc(cliff_jag, (size_t)RH * sizeof *cliff_jag);
    hill_jag = xrealloc(hill_jag, (size_t)RH * sizeof *hill_jag);
    mist = xrealloc(mist, (size_t)RH * sizeof *mist);
    far_top = xrealloc(far_top, (size_t)RW * sizeof *far_top);
    near_top = xrealloc(near_top, (size_t)RW * sizeof *near_top);
    snow = xrealloc(snow, (size_t)RW * sizeof *snow);
    valley = xrealloc(valley, op * sizeof *valley);
    soft = xrealloc(soft, op * sizeof *soft);
    out = xrealloc(out, op * sizeof *out);
    if (!col || !mat || !zbuf || !row_claimed || !row_filled || !row_suffix || !sky || !cliff_jag || !hill_jag
        || !far_top
        || !near_top || !snow || !valley || !soft || !out) {
        OK = 0;
        return false;
    }
    return true;
}

#define MAX_BANDS 64
static int nbands, band_edge[MAX_BANDS + 1];   /* output rows */

static void plan_bands(void)
{
    nbands = OH / 8 < 24 ? OH / 8 : 24;
    if (nbands < 1) nbands = 1;
    for (int i = 0; i <= nbands; i++) band_edge[i] = OH * i / nbands;
}

static void render_band(int i, void *ctx)
{
    (void)ctx;
    int o0 = band_edge[i], o1 = band_edge[i + 1], b0 = o0 * SSF, b1 = o1 * SSF;
    clear_rows(b0, b1);
    draw_road(b0, b1);
    draw_background(b0, b1);
    draw_objects(b0, b1);
    resolve_rows(o0, o1);
}

static void sharpen_band(int i, void *ctx)
{
    (void)ctx;
    sharpen_rows(band_edge[i], band_edge[i + 1]);
}

static void cover_rect(int x0, int y0, int w, int h)
{
    for (int y = y0; y < y0 + h; y++) {
        if (y < 0 || y >= 200) continue;
        for (int x = x0; x < x0 + w; x++)
            if (x >= 0 && x < 320) cover[y * 320 + x] = 1;
    }
}

static void cover_mask(FarPtr p, int x, int y, int op)
{
    const Spr *s = spr_get(p);
    if (!s) return;
    Lut lut[16];
    make_lut(s, op, lut);
    for (int j = 0; j < s->h; j++) {
        int yy = y + j;
        if (yy < 0 || yy >= 200) continue;
        for (int i = 0; i < s->w; i++) {
            int xx = x + i;
            if (xx < 0 || xx >= 320) continue;
            const Lut *L = &lut[s->bits[j * s->w + i]];
            if (op == OP_COPY || L->a != 7 || L->o) cover[yy * 320 + xx] = 1;
        }
    }
}

/* What draw_buffer_overlays (0x349D) puts into the road buffer. */
static void update_cover(void)
{
    memset(cover, 0, sizeof cover);
    cover_rect(240, 0x1B, 80, 0x2C - 0x1B);                       /* mirror */
    const Spr *m = spr_get(far_rd(DGROUP, DS_spr_mirr));
    if (m) cover_mask(far_rd(DGROUP, DS_spr_mirr), m->ox, m->oy, OP_AND);
    if (DSB(DS_cop_state) == 6 && DSB(DS_cop_timer) == 0) {
        const Spr *t = spr_get(far_rd(DGROUP, DS_spr_tick));
        if (t) cover_rect(t->ox, t->oy, t->w, t->h);
    }
    if (DSB(DS_g_stageEvent) == 2 && (DSW(DS_g_stage) != 4 || DSB(DS_ending_shown) != 1)) {
        const char *s = (const char *)mp(DGROUP, DSW(DS_g_stage) != 4 ? 0x1FF7 : 0x2019);
        int len = (int)strlen(s);
        cover_rect(0xA0 - 4 * len, 0x50, 8 * len, DSW(DS_text_glyph_h));
    }
    for (int k = 0; k < 4; k++) memcpy(vram_snap[k], gfx_ega_plane(k) + WIN_Y0 * 40, sizeof vram_snap[k]);
}

/* Test Drive II's distance and time readout (its ROAD.PES dgt0..9 and <car>DASH.PES "time" sprites, at the
 * positions most of its cars' .BIN files give): miles to the end of the stage in tenths and the stage clock,
 * light blue, "MILES" and the separators light green, in a red frame on black. */
static const u8 TD2_DIGITS[10][5] = {
    { 0xF, 0x9, 0x9, 0x9, 0xF }, { 0x1, 0x1, 0x1, 0x1, 0x1 }, { 0xF, 0x1, 0xF, 0x8, 0xF },
    { 0xF, 0x1, 0xF, 0x1, 0xF }, { 0x9, 0x9, 0xF, 0x1, 0x1 }, { 0xF, 0x8, 0xF, 0x1, 0xF },
    { 0x8, 0x8, 0xF, 0x9, 0xF }, { 0xF, 0x1, 0x1, 0x1, 0x1 }, { 0xF, 0x9, 0xF, 0x9, 0xF },
    { 0xF, 0x9, 0xF, 0x1, 0x1 },
};
static const char *const TD2_TIME[5] = {                  /* the "time" sprite at (248, 3) */
    "                      ##### # #   ### ###                 ",
    "                      # # # # #   #   #                  #",
    "                      # # # # #   ### ###                 ",
    "                      # # # # #   #     #                #",
    "              #       # # # # ### ### ###                 ",
};
#define TD2_UNITS_PER_TENTH 48                            /* 480 road units a mile (the results' 0x5A / 12 Hz) */

/* k x k block at original coordinates (x, y) */
static void block(u32 *px, int k, int x, int y, u32 c)
{
    for (int j = 0; j < k; j++)
        for (int i = 0; i < k; i++) px[(size_t)(y * k + j) * 320 * k + x * k + i] = c;
}

static void td2_digit(u32 *px, int k, int x, int d)
{
    for (int y = 0; y < 5; y++)
        for (int i = 0; i < 4; i++)
            if (TD2_DIGITS[d][y] & (8 >> i)) block(px, k, x + 1 + i, 3 + y, 0x5555FF);
}

static void draw_timer(u32 *px, int k)
{
    for (int y = 0; y <= 10; y++)
        for (int x = 246; x <= 319; x++) {
            bool frame = x >= 247 && x <= 318 && y >= 1 && y <= 9 && (x == 247 || x == 318 || y == 1 || y == 9);
            block(px, k, x, y, frame ? 0xAA0000 : 0x000000);
        }
    for (int y = 0; y < 5; y++)
        for (int x = 0; TD2_TIME[y][x]; x++)
            if (TD2_TIME[y][x] == '#') block(px, k, 248 + x, 3 + y, 0x55FF55);

    int left = (int)stage_end - (int)DSW(DS_road_pos);
    int tenths = left > 0 ? left / TD2_UNITS_PER_TENTH : 0;
    if (tenths > 999) tenths = 999;
    td2_digit(px, k, 251, tenths / 100);
    td2_digit(px, k, 256, tenths / 10 % 10);
    td2_digit(px, k, 263, tenths % 10);

    int secs = DSW(DS_g_stageTime) / 12;                  /* the results screen's seconds */
    int mm = secs / 60 > 99 ? 99 : secs / 60;
    td2_digit(px, k, 294, mm / 10);
    td2_digit(px, k, 299, mm % 10);
    td2_digit(px, k, 306, secs % 60 / 10);
    td2_digit(px, k, 311, secs % 60 % 10);
}

/* Crack lines at output resolution, about half an original pixel wide. */
static void draw_cracks(u32 *px, int k)
{
    u32 c = gfx_palette_rgb(15);
    int w = k / 2 > 1 ? k / 2 : 1, ow = 320 * k;
    for (int i = 0; i < ncracks; i++) {
        int x0 = cracks[i].x0 * k + k / 2, y0 = cracks[i].y0 * k + k / 2;
        int x1 = cracks[i].x1 * k + k / 2, y1 = cracks[i].y1 * k + k / 2;
        int dx = abs(x1 - x0), dy = -abs(y1 - y0), sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1, err = dx + dy;
        for (;;) {
            for (int j = 0; j < w; j++)
                for (int m = 0; m < w; m++) {
                    int x = x0 + m - w / 2, y = y0 + j - w / 2;
                    if (x < 0 || x >= ow || y < WIN_Y0 * k || y >= WIN_Y1 * k || cover[(y / k) * 320 + x / k]) continue;
                    px[(size_t)y * ow + x] = c;
                }
            if (x0 == x1 && y0 == y1) break;
            int e2 = 2 * err;
            if (e2 >= dy) { err += dy; x0 += sx; }
            if (e2 <= dx) { err += dx; y0 += sy; }
        }
    }
}

static bool ov_dirty(void)
{
    bool d = dirty;
    dirty = false;
    return d;
}

static void ov_draw(u32 *px, int k)
{
    if (!active || k != OK) return;
    uint64_t t0 = host_time_ns();
    for (int y = WIN_Y0; y < WIN_Y1; y++) {
        const u8 *p0 = gfx_ega_plane(0) + y * 40, *p1 = gfx_ega_plane(1) + y * 40;
        const u8 *p2 = gfx_ega_plane(2) + y * 40, *p3 = gfx_ega_plane(3) + y * 40;
        const u8 *s0 = vram_snap[0] + (y - WIN_Y0) * 40, *s1 = vram_snap[1] + (y - WIN_Y0) * 40;
        const u8 *s2 = vram_snap[2] + (y - WIN_Y0) * 40, *s3 = vram_snap[3] + (y - WIN_Y0) * 40;
        for (int bx = 0; bx < 40; bx++) {
            u8 changed = (u8)((p0[bx] ^ s0[bx]) | (p1[bx] ^ s1[bx]) | (p2[bx] ^ s2[bx]) | (p3[bx] ^ s3[bx]));
            for (int b = 0; b < 8; b++) {
                int x = bx * 8 + b;
                if (cover[y * 320 + x] || (changed & (0x80 >> b))) continue;
                for (int j = 0; j < k; j++) {
                    u32 *d = px + (size_t)(y * k + j) * OW + (size_t)x * k;
                    const u32 *s = out + (size_t)((y - WIN_Y0) * k + j) * OW + (size_t)x * k;
                    for (int i = 0; i < k; i++) d[i] = s[i];
                }
            }
        }
    }
    draw_cracks(px, k);
    draw_timer(px, k);
    stats_add(&stats_overlay_ms, t0);
}

/* ------------------------------------------------------------------------------------------------ */
/* public                                                                                           */

void enh_init(void)
{
    init_luts();
    stats_on = getenv("TD_ENH_STATS") != NULL;
    gfx_set_overlay(ov_dirty, ov_draw);
}

void enh_stage_begin(void)
{
    cache_clear();
    active = false;
    sm_valid = false;
    last_ns = 0;
    world_pos = DSW(DS_road_pos);
    stage_end = world_pos;                                /* the first end marker 0x28 units ahead, as 0x4241 */
    for (u32 u = (u32)world_pos + 0x28; u <= 0xFFFF; u++)
        if (DSB((u16)u) == 0xFF) { stage_end = (u16)(u - 0x28); break; }
    world_head = world_x = world_z = 0;
    emu_next_ns = 0;
    track_reset();
    ncracks = 0;
    memset(fades, 0, sizeof fades);
    dirty = true;
}

void enh_stage_end(void)
{
    active = false;
    ncracks = 0;
    cache_clear();
    dirty = true;
}

void enh_life_reset(void)
{
    sm_valid = false;
    ncracks = 0;
    memset(fades, 0, sizeof fades);
    track_reset();
}

void enh_frame(void)
{
    if (!ensure_buffers()) return;
    uint64_t t0 = host_time_ns();
    update_view();
    walk_road();
    prepare_road();
    prepare_background();
    collect_objects();
    sort_objects();
    plan_bands();
    host_parallel_for(nbands, render_band, NULL);
    host_parallel_for(nbands, sharpen_band, NULL);
    update_cover();
    active = true;
    dirty = true;
    if (stats_on) {
        double ms = (double)(host_time_ns() - t0) / 1e6;
        stats_render_ms += ms;
        if (ms > stats_render_max) stats_render_max = ms;
        stats_report();
    }
}

void enh_gear_box(void)
{
    u8 dt = DSB(DS_dash_timer);
    draw_gear_box();
    if (!emu_frame && dt > 1 && DSB(DS_dash_timer) == (u8)(dt - 1)) DSB(DS_dash_timer) = dt;
}

static void add_crack(u16 a, u16 b)
{
    if (ncracks == (int)(sizeof cracks / sizeof cracks[0])) return;
    cracks[ncracks++] = (Crack){ (s16)(DSB(a) << 1), DSB((u16)(a + 1)), (s16)(DSB(b) << 1), DSB((u16)(b + 1)) };
}

/* 0x392C with the road left as rendered: the seven crack sets appear at the original 8 Hz. */
void enh_crash_sequence(void)
{
    int fr = host_frame_rate();
    host_set_frame_rate(8);
    ncracks = 0;
    for (u16 set = 0; set < 7; set++) {
        host_frame_begin();
        u16 bx = DSW((u16)(DS_crack_ptrs + (set << 1)));
        u8 cnt = DSB((u16)(DS_crack_counts + set));
        u16 si = set == 0 ? 2 : 0;
        do {
            if (set == 0) { add_crack(bx, (u16)(bx + si)); si = (u16)(si + 2); }
            else { add_crack((u16)(bx + si), (u16)(bx + si + 2)); si = (u16)(si + 4); }
        } while (--cnt != 0);
        draw_mirror_background();
        draw_road_mirror();
        draw_buffer_overlays();
        present_road_buffer();
        update_cover();
        dirty = true;
    }
    host_set_frame_rate(fr);
    if (DSW(DS_g_lives) != 1) wait_fire_button();
}

void enh_cover_sprite(FarPtr mask)
{
    const Spr *s = spr_get(mask);
    if (s) cover_mask(mask, s->ox, s->oy, OP_AND);
    dirty = true;
}
