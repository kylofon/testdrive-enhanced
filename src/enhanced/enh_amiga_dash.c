/* AMIGA dashboards (--amiga dashboards): the Amiga cockpit of the car being driven, as a 320 x 200 layer laid over
 * the enhanced view (enhanced.c), composed every frame from the DOS game's state (port/amiga/spec/drive_scene.md
 * in test-drive-sdl3, §1.1 and §4.7-§4.9):
 *   - rows 117..199: the car's dash picture (Cars/<car>Dash, its own palette), the steering wheel's pose and the
 *     gear gate XOR-drawn over it in colour indices, as the Amiga does; the needles (analog gauges, its sprites
 *     0..3, colours 17 and 21) from the pivot to the tip of the car record's tables, under the wheel rim
 *     (tcm*, spm*); the Corvette's digital cluster (inst, sped, tach, the dgt digits); the gear knob (gnob, sprites
 *     4..7, colours 16..31) and the steering-wheel marker (a 4 x 4 dot, colour 31);
 *   - rows 0..116: hood, roof, post and mirror frame (road palette) and the radar detector's lights (rad*).
 * The needle tips, pivots and the knob's and gate's places are the same in the DOS record (DS:268F) and the
 * Amiga one (FORMATS.md, "Car record"); the marker's circle and the digital cluster's fields are read from the
 * Amiga record. */
#include "enh_amiga_dash.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../amiga/abridge.h"
#include "../symbols.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define DASH_TOP 117                        /* 0x75: the palette split, the dash below */
#define CAR(o)   ((u16)(0x268F + (o)))       /* the DOS car record */

static bool active;
static const AmigaPicture *pic;
static const AmigaImage *hood, *roof, *post, *mirr, *whl[4], *gbox, *gnob, *rad[6], *tcm[5], *spm[5];
static const AmigaImage *inst, *sped, *tach, *dgs[10], *dgt[10];
static u8 *rec;                             /* the Amiga car record (big-endian) */
static u32 rec_len;
static u32 layer[320 * 200];
static u8 ix[320 * 200];                    /* colour indices of the dash, rows DASH_TOP.. */
static s16 wix[320 * DASH_TOP];             /* colour indices of the window's cockpit parts, -1 = none */

static u16 rw(u32 o) { return o + 1 < rec_len ? (u16)(rec[o] << 8 | rec[o + 1]) : 0; }
static u8 rb(u32 o) { return o < rec_len ? rec[o] : 0; }

bool amiga_dash_active(void) { return active; }

/* The Amiga car file ("cars/P911t") of the DOS car being driven: CARS.TXT names "p911t", "counta". */
static const char *car_file(void)
{
    const char *dos = (const char *)mp(DGROUP, DSW((u16)(DS_g_carNames + DSS(DS_g_selectedCar) * 2)));
    size_t n = strlen(dos);
    for (int i = 0; i < amiga_car_count() && n; i++) {
        const char *f = amiga_car_file(i), *base = strrchr(f, '/');
        base = base ? base + 1 : f;
        size_t k = 0;
        while (k < n && base[k] && (base[k] | 0x20) == (dos[k] | 0x20)) k++;
        if (k == n) return f;
    }
    return NULL;
}

bool amiga_dash_begin(void)
{
    active = false;
    if (!amiga_ready()) return false;
    const char *file = car_file();
    if (!file) return false;
    char path[64], shp[64], name[5];
    snprintf(path, sizeof path, "%sDash", file);
    snprintf(shp, sizeof shp, "%sDash.Shp", file);
    pic = amiga_picture(path);
    if (!pic || pic->w < 320 || pic->h < 200) return false;
    free(rec);
    snprintf(path, sizeof path, "%s.B", file);
    rec = amiga_file(path, &rec_len);
    if (!rec) return false;
    const u16 *dp = pic->palette;
    /* the window's parts are in the road palette (above the palette split), the dash's in the dash's own */
    hood = amiga_image(shp, "hood", NULL);
    roof = amiga_image(shp, "roof", NULL);
    post = amiga_image(shp, "post", NULL);
    mirr = amiga_image(shp, "mirr", NULL);
    for (int i = 0; i < 6; i++) { snprintf(name, sizeof name, "rad%d", i); rad[i] = amiga_image(shp, name, NULL); }
    for (int i = 0; i < 4; i++) { snprintf(name, sizeof name, "whl%d", i); whl[i] = amiga_image(shp, name, dp); }
    gbox = amiga_image(shp, "gbox", dp);
    gnob = amiga_image(shp, "gnob", dp);
    static const char *const TCM[5] = { "tcm0", "tcm1", "tcmC", "tcm2", "tcm3" };
    static const char *const SPM[5] = { "spm0", "spm1", "spmC", "spm2", "spm3" };
    for (int i = 0; i < 5; i++) {
        tcm[i] = amiga_image(shp, TCM[i], dp);
        spm[i] = amiga_image(shp, SPM[i], dp);
    }
    for (int i = 0; i < 5; i++) {                    /* missing poses: the centred one */
        if (!tcm[i]) tcm[i] = tcm[2];
        if (!spm[i]) spm[i] = spm[2];
    }
    inst = sped = tach = NULL;
    memset(dgs, 0, sizeof dgs);
    memset(dgt, 0, sizeof dgt);
    if (DSW(CAR(0x16C)) != 1) {                      /* the digital cluster (Corvette) */
        inst = amiga_image(shp, "inst", dp);
        sped = amiga_image(shp, "sped", dp);
        tach = amiga_image(shp, "tach", dp);
        for (int d = 0; d < 10; d++) {
            char n1[5] = { (char)(rb(0x182) & 0x7F), (char)(rb(0x183) & 0x7F), (char)(rb(0x184) & 0x7F), (char)('0' + d), 0 };
            char n2[5] = { (char)(rb(0x32C) & 0x7F), (char)(rb(0x32D) & 0x7F), (char)(rb(0x32E) & 0x7F), (char)('0' + d), 0 };
            dgs[d] = amiga_image(shp, n1, dp);
            dgt[d] = amiga_image(shp, n2, dp);
        }
    }
    active = true;
    return true;
}

void amiga_dash_end(void)
{
    active = false;
    free(rec);
    rec = NULL;
    rec_len = 0;
}

/* ---- drawing into the colour-index buffers */

/* XOR a shape's indices into ix at (x, y), only rows clip_top.. and columns ..clip_right */
static void xor_ix(const AmigaImage *s, int x, int y, int clip_top, int clip_right)
{
    if (!s) return;
    for (int j = 0; j < s->h; j++) {
        int yy = y + j;
        if (yy < DASH_TOP || yy < clip_top || yy >= 200) continue;
        for (int i = 0; i < s->w; i++) {
            int xx = x + i;
            if (xx < 0 || xx >= 320 || xx > clip_right) continue;
            ix[yy * 320 + xx] ^= s->idx[j * s->w + i];
        }
    }
}

/* the shape's opaque pixels replace ix (copy: all of its pixels, colour 0 too) */
static void put_ix(const AmigaImage *s, int x, int y, bool copy)
{
    if (!s) return;
    for (int j = 0; j < s->h; j++) {
        int yy = y + j;
        if (yy < DASH_TOP || yy >= 200) continue;
        for (int i = 0; i < s->w; i++) {
            int xx = x + i;
            u8 v = s->idx[j * s->w + i];
            if (xx < 0 || xx >= 320 || (!v && !copy)) continue;
            ix[yy * 320 + xx] = v;
        }
    }
}

/* the window's parts: the opaque pixels replace (or XOR) wix */
static void put_wix(const AmigaImage *s, int x, int y, bool xor_)
{
    if (!s) return;
    for (int j = 0; j < s->h; j++) {
        int yy = y + j;
        if (yy < 0 || yy >= DASH_TOP) continue;
        for (int i = 0; i < s->w; i++) {
            int xx = x + i;
            u8 v = s->idx[j * s->w + i];
            if (xx < 0 || xx >= 320 || !v) continue;
            s16 *p = &wix[yy * 320 + xx];
            *p = xor_ ? (s16)((*p < 0 ? 0 : *p) ^ v) : v;
        }
    }
}

/* draw_needle 0x1F2EA: the needle is ANDed with plane 0 of the wheel-rim overlay (tcm*, spm*), read from the
 * needle box's origin (bx, by) (+172..+178), not the overlay's own place: it shows where that bit is set. */
static bool visible_at(const AmigaImage *rim, int bx, int by, int x, int y)
{
    if (!rim) return true;
    int i = x - bx, j = y - by;
    return i >= 0 && j >= 0 && i < rim->w && j < rim->h && (rim->idx[j * rim->w + i] & 1);
}

/* A needle from (x0, y0) to (x1, y1) in colour c, through the rim's stencil. */
static void needle(int x0, int y0, int x1, int y1, u32 c, const AmigaImage *rim, int bx, int by)
{
    int dx = abs(x1 - x0), dy = -abs(y1 - y0), sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1, err = dx + dy;
    for (int n = 0; n < 400; n++) {
        if (x0 >= 0 && x0 < 320 && y0 >= DASH_TOP && y0 < 200 && visible_at(rim, bx, by, x0, y0))
            layer[y0 * 320 + x0] = c;
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

/* draw_number 0x20A88: hundreds, tens and units at the record's three x,y pairs (top-left) */
static void number(u16 v, const AmigaImage *const *dg, u32 xy)
{
    if (v >= 100) xor_ix(dg[v / 100 % 10], (s16)rw(xy), (s16)rw(xy + 2), 0, 319);
    if (v >= 10) xor_ix(dg[v / 10 % 10], (s16)rw(xy + 4), (s16)rw(xy + 6), 0, 319);
    xor_ix(dg[v % 10], (s16)rw(xy + 8), (s16)rw(xy + 10), 0, 319);
}

static int sx_of(const AmigaImage *s) { return s ? s->x & ~15 : 0; }
static int sy_of(const AmigaImage *s) { return s ? s->y : 0; }

const u32 *amiga_dash_layer(void)
{
    if (!active) return NULL;
    const u16 *dp = pic->palette;
    u16 mph = DSB((u16)(DS_g_carSpeedFixed + 1)), rpm = DSW(DS_rpm);
    /* The DOS steering angle turns the other way round from the Amiga's (positive = left): negated, the wheel's
     * pose and the marker turn with the car. */
    int steer = -DSS(DS_steer_angle);

    /* the wheel's pose: 0..4, 2 centred (draw_steering_wheel; the Amiga's degrees are the DOS angle / 128) */
    int p = (abs(steer) / 128 + 4) >> 3;
    if (p > 2) p = 2;
    if (steer < 0) p = -p;
    p += 2;
    bool gearbox = DSB(DS_gearbox_display_toggle) == 1 || DSB(DS_dash_timer) != 0;

    /* the dash, in colour indices */
    memcpy(ix + DASH_TOP * 320, pic->index + DASH_TOP * 320, (size_t)(200 - DASH_TOP) * 320);
    if (p != 2) {
        const AmigaImage *w = whl[p > 2 ? p - 1 : p];
        xor_ix(w, sx_of(w), sy_of(w), 0, 319);
    }
    if (gearbox) xor_ix(gbox, sx_of(gbox), sy_of(gbox), 0, 319);
    bool digital = DSW(CAR(0x16C)) != 1;
    if (digital && inst) {                           /* draw_digital_cluster 0x2097C */
        put_ix(inst, sx_of(inst), sy_of(inst), true);
        int len = (int)((u32)mph * rw(0x180) >> 16);
        if (rw(0x17A) == 0) xor_ix(sped, sx_of(sped), sy_of(sped), (s16)rw(0x17C) - len, 319);
        else xor_ix(sped, sx_of(sped), sy_of(sped), 0, (s16)rw(0x17C) + len);
        u16 r = rpm < 0x320 ? 0x320 : rpm;
        len = (int)((u32)(r >> 4) * rw(0x32A) >> 16);
        if (rw(0x324) == 0) xor_ix(tach, sx_of(tach), sy_of(tach), (s16)rw(0x326) - len, 319);
        else xor_ix(tach, sx_of(tach), sy_of(tach), 0, (s16)rw(0x326) + len);
        number(mph, dgs, 0x16E);
        u16 t = (u16)(r / 100);
        number(t >= 100 ? 99 : t, dgt, 0x318);        /* +318: hundreds (never drawn), tens, units */
        put_ix(tcm[p], tcm[p] ? tcm[p]->x : 0, tcm[p] ? tcm[p]->y : 0, false);
        put_ix(spm[p], spm[p] ? spm[p]->x : 0, spm[p] ? spm[p]->y : 0, false);
    }
    for (int y = DASH_TOP; y < 200; y++)
        for (int x = 0; x < 320; x++) layer[y * 320 + x] = 0xFF000000u | amiga_rgb(dp[ix[y * 320 + x] & 31]);

    /* needles (update_gauges 0x20AF4, draw_needle 0x1F2EA): pivot +170 / +16E, tips +328 / +17A */
    if (!digital) {
        u16 r = rpm > DSW(CAR(0x002)) ? DSW(CAR(0x002)) : rpm;
        if (r < 0x320) r = 0x320;
        u16 i = (u16)((r >> 5) & 0x1FE);
        needle(DSB(CAR(0x170)), DSB(CAR(0x171)), DSB((u16)(CAR(0x328) + i)), DSB((u16)(CAR(0x328) + i + 1)),
               0xFF000000u | amiga_rgb(dp[17]), tcm[p], DSW(CAR(0x176)), DSW(CAR(0x178)));
        int v = mph - 10;
        if (v < 0) v = 0;
        if (v > 214) v = 214;
        needle(DSB(CAR(0x16E)), DSB(CAR(0x16F)), DSB((u16)(CAR(0x17A) + 2 * v)), DSB((u16)(CAR(0x17A) + 2 * v + 1)),
               0xFF000000u | amiga_rgb(dp[21]), spm[p], DSW(CAR(0x172)), DSW(CAR(0x174)));
    }

    /* the gear knob (sprites 4..7: gnob's planes 0..3 in colours 16..31), while the gate is shown */
    if (gearbox && gnob) {
        int kx = (s16)DSW(DS_knob_x), ky = (s16)DSW(DS_knob_y);
        for (int j = 0; j < gnob->h; j++)
            for (int i = 0; i < gnob->w; i++) {
                int x = kx + i, y = ky + j;
                u8 v = gnob->idx[j * gnob->w + i] & 15;
                if (v && x >= 0 && x < 320 && y >= DASH_TOP && y < 200)
                    layer[y * 320 + x] = 0xFF000000u | amiga_rgb(dp[16 + v]);
            }
    }

    /* the window's parts (draw_cockpit_overlays 0x1EBC4), road palette */
    for (int i = 0; i < 320 * DASH_TOP; i++) wix[i] = -1;
    put_wix(hood, sx_of(hood), sy_of(hood), false);
    put_wix(roof, sx_of(roof), sy_of(roof), false);
    put_wix(post, sx_of(post), sy_of(post), false);
    put_wix(mirr, sx_of(mirr), sy_of(mirr), false);
    /* the radar detector: the DOS game's lamps (the trap's distance, blinking with the stage clock) */
    if (DSB(DS_g_stageTime) & 8) {
        u16 bx = DSW(DS_snapshot_radar_trap_pos);
        if (bx != 0 && (s16)(u16)(bx - 1) >= (s16)DSW(DS_r_road_ptr)) {
            int l = 5 - ((u16)(bx - 1 - DSW(DS_r_road_ptr)) >> 4);
            if (l < 1) l = 1;
            put_wix(rad[l], sx_of(rad[l]), sy_of(rad[l]), true);
        }
    }
    for (int i = 0; i < 320 * DASH_TOP; i++)
        layer[i] = wix[i] < 0 ? 0 : 0xFF000000u | amiga_rgb(AMIGA_ROAD_PALETTE[wix[i] & 31]);

    /* the steering-wheel marker (update_wheel_marker 0x247B2): +00C, +00E centre, +010 radius, +-35 degrees */
    double a = steer / 64.0;
    if (a > 35) a = 35;
    if (a < -35) a = -35;
    int rr = (s16)rw(0x010);
    int mx = (s16)rw(0x00C) + (int)lround(sin(a * M_PI / 180.0) * rr);
    int my = (s16)rw(0x00E) - (int)lround(cos(a * M_PI / 180.0) * rr);
    static const u8 DOT[4] = { 0x6, 0xF, 0xF, 0x6 };
    for (int j = 0; j < 4; j++)
        for (int i = 0; i < 4; i++)
            if (DOT[j] & (8 >> i)) {
                int x = mx + i, y = my + j;
                if (x >= 0 && x < 320 && y >= 0 && y < 200) layer[y * 320 + x] = 0xFF000000u | amiga_rgb(dp[31]);
            }
    return layer;
}
