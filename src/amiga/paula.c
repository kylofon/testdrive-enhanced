/* Paula: see paula.h. */
#include "paula.h"

#include <math.h>
#include <string.h>

#include "ahost.h"

typedef struct {
    APTR lc;                /* registers */
    u16 len, per, vol;
    APTR ptr;               /* internal: current pass */
    u32 bytes, pos;         /* bytes in the pass, byte being played */
    double phase;           /* fraction of the current byte played */
    s8 sample;              /* output held since the last byte */
} Channel;

static Channel chan[4];
static u16 dmacon, intena, intreq;
static void (*level4)(void);
static bool led_filter = true;

/* Filters per side: the fixed one-pole RC (~4.4 kHz) and the LED 2-pole Butterworth (~3.3 kHz). */
static double rc_a;
static struct { double b0, b1, b2, a1, a2; } led;
static double rc_state[2], led_x1[2], led_x2[2], led_y1[2], led_y2[2];

void paula_set_lc(int ch, APTR lc)      { chan[ch & 3].lc = lc; }
void paula_set_len(int ch, u16 words)   { chan[ch & 3].len = words; }
void paula_set_per(int ch, u16 period)  { chan[ch & 3].per = period; }
void paula_set_vol(int ch, u16 vol)     { chan[ch & 3].vol = vol > 64 ? 64 : vol; }
void paula_set_level4_handler(void (*handler)(void)) { level4 = handler; }
void paula_set_led_filter(bool on) { led_filter = on; }

static u32 pass_bytes(const Channel *c) { return (c->len ? c->len : 0x10000u) * 2u; }

static void raise(int ch)
{
    intreq |= (u16)(0x80 << ch);
    if (level4 && (intreq & intena & 0x0780)) level4();
}

static void load_pass(int ch)
{
    Channel *c = &chan[ch];
    c->ptr = c->lc;
    c->bytes = pass_bytes(c);
    c->pos = 0;
    c->phase = 0;
}

void paula_dmacon(u16 v)
{
    u16 old = dmacon;
    if (v & 0x8000) dmacon |= v & 0x7FFF;
    else dmacon &= (u16)~v;
    for (int ch = 0; ch < 4; ch++) {
        u16 bit = (u16)(1 << ch);
        if (!(old & bit) && (dmacon & bit)) {          /* DMA on: load LC/LEN, request the interrupt */
            load_pass(ch);
            chan[ch].sample = (s8)rd8(chan[ch].ptr);
            raise(ch);
        } else if ((old & bit) && !(dmacon & bit)) {
            chan[ch].sample = 0;
        }
    }
}

u16 paula_dmaconr(void) { return dmacon; }

void paula_intena(u16 v)
{
    if (v & 0x8000) intena |= v & 0x7FFF;
    else intena &= (u16)~v;
}

u16 paula_intenar(void) { return intena; }

void paula_intreq(u16 v)
{
    if (v & 0x8000) intreq |= v & 0x7FFF;
    else intreq &= (u16)~v;
}

u16 paula_intreqr(void) { return intreq; }

/* One output frame: advance every channel by 1/AUDIO_RATE s. */
static void step(int *left, int *right)
{
    int out[4] = { 0, 0, 0, 0 };
    for (int ch = 0; ch < 4; ch++) {
        Channel *c = &chan[ch];
        if (!(dmacon & (1 << ch))) continue;
        u16 per = c->per < 124 ? 124 : c->per;        /* the DMA cannot fetch faster */
        c->phase += (double)PAULA_CLOCK / per / AUDIO_RATE;
        while (c->phase >= 1.0 && (dmacon & (1 << ch))) {
            c->phase -= 1.0;
            if (++c->pos >= c->bytes) {                /* end of the pass: reload from the registers */
                double ph = c->phase;
                load_pass(ch);
                c->phase = ph;
                raise(ch);
                if (!(dmacon & (1 << ch))) break;      /* the handler stopped it */
            }
            c->sample = (s8)rd8(c->ptr + c->pos);
        }
        if (dmacon & (1 << ch)) out[ch] = c->sample * c->vol;
    }
    *left = out[0] + out[3];
    *right = out[1] + out[2];
}

static double filter(int side, double x)
{
    rc_state[side] += rc_a * (x - rc_state[side]);
    double y = rc_state[side];
    if (!led_filter) return y;
    double z = led.b0 * y + led.b1 * led_x1[side] + led.b2 * led_x2[side] - led.a1 * led_y1[side] - led.a2 * led_y2[side];
    led_x2[side] = led_x1[side]; led_x1[side] = y;
    led_y2[side] = led_y1[side]; led_y1[side] = z;
    return z;
}

static void render(s16 *lr, int frames)
{
    for (int i = 0; i < frames; i++) {
        int l, r;
        step(&l, &r);
        double fl = filter(0, l * 2.0), fr = filter(1, r * 2.0);   /* each side at most +-16384 before x2 */
        lr[2 * i] = (s16)(fl > 32767 ? 32767 : fl < -32768 ? -32768 : fl);
        lr[2 * i + 1] = (s16)(fr > 32767 ? 32767 : fr < -32768 ? -32768 : fr);
    }
}

void paula_init(void)
{
    memset(chan, 0, sizeof chan);
    dmacon = intena = intreq = 0;
    const double pi = 3.14159265358979323846;
    rc_a = 1.0 - exp(-2.0 * pi * 4420.0 / AUDIO_RATE);
    double k = tan(pi * 3275.0 / AUDIO_RATE), q = sqrt(0.5);
    double norm = 1.0 / (1.0 + k / q + k * k);
    led.b0 = k * k * norm;
    led.b1 = 2.0 * led.b0;
    led.b2 = led.b0;
    led.a1 = 2.0 * (k * k - 1.0) * norm;
    led.a2 = (1.0 - k / q + k * k) * norm;
    ahost_set_audio_source(render);
}
