/* The Amiga engine's host (ahost.h) on top of the DOS game's host (../host.h): one window, one keyboard, one
 * gamepad. test-drive-sdl3's tdport-amiga has its own SDL host; here the engine borrows the game's.
 *
 * - The 60 Hz vertical blank runs from every host_pump() (host_set_pump_hook), so the Amiga songs keep playing
 *   while the DOS game runs its own loops.
 * - The engine's audio goes to a stream of its own on the default device; SDL mixes it with the speaker's.
 * - Its screens are shown only while amiga_screens_begin() .. amiga_screens_end() (amiga_bridge.c): the game's
 *   frame source is saved and the engine's picture shown instead, and every key goes to the engine's input
 *   handler instead of the BIOS buffer. */
#include "ahost.h"

#include <SDL3/SDL.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "../host.h"
#include "abridge.h"

static SDL_AudioStream *audio;

static void (*frame_source)(u32 *);
static int frame_w = 640, frame_h = 200;
static bool showing;                  /* the engine's screens are shown */

static void (*audio_source)(s16 *, int);
static void (*key_handler)(u8, u16);

#define MAX_SERVERS 8
static struct { int pri; void (*fn)(void); } servers[MAX_SERVERS];
static int nservers;

/* Tick clock: tick n is due at start + n / 60 s. */
static u64 clock_start_ns;
static u32 ticks_run;
static bool running;

static void pump_hook(void);
static bool key_hook(int scancode, int mod, bool down, bool repeat);

bool ahost_init(int window_scale)
{
    (void)window_scale;
    SDL_AudioSpec spec = { SDL_AUDIO_S16, 2, AUDIO_RATE };
    audio = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, NULL, NULL);
    if (audio) {
        SDL_ResumeAudioStreamDevice(audio);
        static s16 silence[AUDIO_RATE / 20 * 2];               /* 50 ms of lead-in against underruns */
        SDL_PutAudioStreamData(audio, silence, sizeof silence);
    } else {
        fprintf(stderr, "Amiga audio unavailable: %s\n", SDL_GetError());
    }
    clock_start_ns = SDL_GetTicksNS();
    ticks_run = 0;
    running = true;
    host_set_pump_hook(pump_hook);
    return true;
}

void ahost_shutdown(void)
{
    running = false;
    host_set_pump_hook(NULL);
    host_set_key_hook(NULL);
    if (audio) SDL_DestroyAudioStream(audio);
    audio = NULL;
}

/* ---------------------------------------------------------------- vertical blank */

void ahost_add_vbl_server(int pri, void (*fn)(void))
{
    if (nservers == MAX_SERVERS) return;
    int i = nservers;
    while (i > 0 && servers[i - 1].pri < pri) { servers[i] = servers[i - 1]; i--; }   /* same priority: FIFO */
    servers[i].pri = pri;
    servers[i].fn = fn;
    nservers++;
}

void ahost_remove_vbl_server(void (*fn)(void))
{
    for (int i = 0; i < nservers; i++)
        if (servers[i].fn == fn) {
            memmove(&servers[i], &servers[i + 1], (size_t)(nservers - i - 1) * sizeof servers[0]);
            nservers--;
            return;
        }
}

u32 ahost_vbl_count(void) { return ticks_run; }

static u64 tick_due_ns(u32 n) { return clock_start_ns + (u64)n * SDL_NS_PER_SECOND / VBL_HZ; }

static void audio_for_one_tick(void)
{
    enum { FRAMES = AUDIO_RATE / VBL_HZ };
    static s16 buf[FRAMES * 2];
    if (audio_source) audio_source(buf, FRAMES);
    else memset(buf, 0, sizeof buf);
    if (!audio) return;
    /* Drop output if the device is far behind (e.g. after a stall) instead of building latency. */
    if (SDL_GetAudioStreamQueued(audio) > AUDIO_RATE / 4 * 4) return;
    SDL_PutAudioStreamData(audio, buf, sizeof buf);
}

static void pump_hook(void)
{
    if (!running) return;
    u64 now = SDL_GetTicksNS();
    int budget = 30;                                    /* at most 0.5 s of catch-up per call */
    while (tick_due_ns(ticks_run + 1) <= now && budget-- > 0) {
        ticks_run++;
        for (int i = 0; i < nservers; i++) servers[i].fn();
        audio_for_one_tick();
    }
    if (budget < 0)                                     /* fell too far behind: resynchronise the clock */
        clock_start_ns = now - (tick_due_ns(ticks_run) - clock_start_ns);
}

void ahost_pump(void) { host_pump(); }

void ahost_wait_tof(void)
{
    u32 start = ticks_run;
    while (ticks_run == start) host_pump();
}

void ahost_delay(u32 fiftieths)
{
    u64 until = SDL_GetTicksNS() + (u64)fiftieths * SDL_NS_PER_SECOND / 50;
    do host_pump(); while (SDL_GetTicksNS() < until);
}

u64 ahost_time_ns(void) { return SDL_GetTicksNS(); }

void ahost_wait_until_ns(u64 t)
{
    do host_pump(); while (SDL_GetTicksNS() < t);
}

/* ---------------------------------------------------------------- display, audio */

static bool compose(u32 *xrgb)
{
    if (!frame_source) return false;
    frame_source(xrgb);
    return true;
}

void ahost_set_frame_source(void (*fn)(u32 *), int w, int h)
{
    frame_source = fn;
    frame_w = SDL_clamp(w, 1, AHOST_FRAME_MAX_W);
    frame_h = SDL_clamp(h, 1, AHOST_FRAME_MAX_H);
    if (showing) host_set_frame_source(compose, frame_w, frame_h);
}

void ahost_set_audio_source(void (*render)(s16 *, int)) { audio_source = render; }

void amiga_screens_begin(void)
{
    if (showing) return;
    showing = true;
    host_save_frame_source();
    host_set_frame_source(compose, frame_w, frame_h);
    host_set_frame_sharp(true);
    host_set_key_hook(key_hook);
}

void amiga_screens_end(void)
{
    if (!showing) return;
    showing = false;
    host_set_key_hook(NULL);
    host_restore_frame_source();
    host_kbd_flush();
}

/* ---------------------------------------------------------------- keyboard, joystick */

void ahost_set_key_handler(void (*handler)(u8, u16)) { key_handler = handler; }

/* Amiga raw key code of an SDL scancode (the positions of the A500 US keyboard); 0xFF = none. */
static u8 amiga_raw_key(SDL_Scancode sc)
{
    static const struct { SDL_Scancode sc; u8 code; } map[] = {
        { SDL_SCANCODE_GRAVE, 0x00 }, { SDL_SCANCODE_1, 0x01 }, { SDL_SCANCODE_2, 0x02 }, { SDL_SCANCODE_3, 0x03 },
        { SDL_SCANCODE_4, 0x04 }, { SDL_SCANCODE_5, 0x05 }, { SDL_SCANCODE_6, 0x06 }, { SDL_SCANCODE_7, 0x07 },
        { SDL_SCANCODE_8, 0x08 }, { SDL_SCANCODE_9, 0x09 }, { SDL_SCANCODE_0, 0x0A }, { SDL_SCANCODE_MINUS, 0x0B },
        { SDL_SCANCODE_EQUALS, 0x0C }, { SDL_SCANCODE_BACKSLASH, 0x0D }, { SDL_SCANCODE_KP_0, 0x0F },
        { SDL_SCANCODE_Q, 0x10 }, { SDL_SCANCODE_W, 0x11 }, { SDL_SCANCODE_E, 0x12 }, { SDL_SCANCODE_R, 0x13 },
        { SDL_SCANCODE_T, 0x14 }, { SDL_SCANCODE_Y, 0x15 }, { SDL_SCANCODE_U, 0x16 }, { SDL_SCANCODE_I, 0x17 },
        { SDL_SCANCODE_O, 0x18 }, { SDL_SCANCODE_P, 0x19 }, { SDL_SCANCODE_LEFTBRACKET, 0x1A },
        { SDL_SCANCODE_RIGHTBRACKET, 0x1B }, { SDL_SCANCODE_KP_1, 0x1D }, { SDL_SCANCODE_KP_2, 0x1E },
        { SDL_SCANCODE_KP_3, 0x1F }, { SDL_SCANCODE_A, 0x20 }, { SDL_SCANCODE_S, 0x21 }, { SDL_SCANCODE_D, 0x22 },
        { SDL_SCANCODE_F, 0x23 }, { SDL_SCANCODE_G, 0x24 }, { SDL_SCANCODE_H, 0x25 }, { SDL_SCANCODE_J, 0x26 },
        { SDL_SCANCODE_K, 0x27 }, { SDL_SCANCODE_L, 0x28 }, { SDL_SCANCODE_SEMICOLON, 0x29 },
        { SDL_SCANCODE_APOSTROPHE, 0x2A }, { SDL_SCANCODE_NONUSHASH, 0x2B }, { SDL_SCANCODE_KP_4, 0x2D },
        { SDL_SCANCODE_KP_5, 0x2E }, { SDL_SCANCODE_KP_6, 0x2F }, { SDL_SCANCODE_NONUSBACKSLASH, 0x30 },
        { SDL_SCANCODE_Z, 0x31 }, { SDL_SCANCODE_X, 0x32 }, { SDL_SCANCODE_C, 0x33 }, { SDL_SCANCODE_V, 0x34 },
        { SDL_SCANCODE_B, 0x35 }, { SDL_SCANCODE_N, 0x36 }, { SDL_SCANCODE_M, 0x37 }, { SDL_SCANCODE_COMMA, 0x38 },
        { SDL_SCANCODE_PERIOD, 0x39 }, { SDL_SCANCODE_SLASH, 0x3A }, { SDL_SCANCODE_KP_PERIOD, 0x3C },
        { SDL_SCANCODE_KP_7, 0x3D }, { SDL_SCANCODE_KP_8, 0x3E }, { SDL_SCANCODE_KP_9, 0x3F },
        { SDL_SCANCODE_SPACE, 0x40 }, { SDL_SCANCODE_BACKSPACE, 0x41 }, { SDL_SCANCODE_TAB, 0x42 },
        { SDL_SCANCODE_KP_ENTER, 0x43 }, { SDL_SCANCODE_RETURN, 0x44 }, { SDL_SCANCODE_ESCAPE, 0x45 },
        { SDL_SCANCODE_DELETE, 0x46 }, { SDL_SCANCODE_KP_MINUS, 0x4A }, { SDL_SCANCODE_UP, 0x4C },
        { SDL_SCANCODE_DOWN, 0x4D }, { SDL_SCANCODE_RIGHT, 0x4E }, { SDL_SCANCODE_LEFT, 0x4F },
        { SDL_SCANCODE_F1, 0x50 }, { SDL_SCANCODE_F2, 0x51 }, { SDL_SCANCODE_F3, 0x52 }, { SDL_SCANCODE_F4, 0x53 },
        { SDL_SCANCODE_F5, 0x54 }, { SDL_SCANCODE_F6, 0x55 }, { SDL_SCANCODE_F7, 0x56 }, { SDL_SCANCODE_F8, 0x57 },
        { SDL_SCANCODE_F9, 0x58 }, { SDL_SCANCODE_F10, 0x59 }, { SDL_SCANCODE_KP_LEFTPAREN, 0x5A },
        { SDL_SCANCODE_KP_RIGHTPAREN, 0x5B }, { SDL_SCANCODE_KP_DIVIDE, 0x5C }, { SDL_SCANCODE_KP_MULTIPLY, 0x5D },
        { SDL_SCANCODE_KP_PLUS, 0x5E }, { SDL_SCANCODE_F11, 0x5F } /* Help */,
        { SDL_SCANCODE_LSHIFT, 0x60 }, { SDL_SCANCODE_RSHIFT, 0x61 }, { SDL_SCANCODE_CAPSLOCK, 0x62 },
        { SDL_SCANCODE_LCTRL, 0x63 }, { SDL_SCANCODE_RCTRL, 0x63 }, { SDL_SCANCODE_LALT, 0x64 },
        { SDL_SCANCODE_RALT, 0x65 }, { SDL_SCANCODE_LGUI, 0x66 }, { SDL_SCANCODE_RGUI, 0x67 },
    };
    for (size_t i = 0; i < SDL_arraysize(map); i++)
        if (map[i].sc == sc) return map[i].code;
    return 0xFF;
}

static u16 qualifier(SDL_Keymod m, u8 code)
{
    u16 q = 0;
    if (m & SDL_KMOD_LSHIFT) q |= IEQUALIFIER_LSHIFT;
    if (m & SDL_KMOD_RSHIFT) q |= IEQUALIFIER_RSHIFT;
    if (m & SDL_KMOD_CAPS)   q |= IEQUALIFIER_CAPSLOCK;
    if (m & SDL_KMOD_CTRL)   q |= IEQUALIFIER_CONTROL;
    if (m & SDL_KMOD_LALT)   q |= IEQUALIFIER_LALT;
    if (m & SDL_KMOD_RALT)   q |= IEQUALIFIER_RALT;
    if (m & SDL_KMOD_LGUI)   q |= IEQUALIFIER_LCOMMAND;
    if (m & SDL_KMOD_RGUI)   q |= IEQUALIFIER_RCOMMAND;
    /* The keypad keys: 0x0F, 0x1D-0x1F, 0x2D-0x2F, 0x3C-0x3F, 0x43, 0x4A, 0x5A-0x5E. */
    if (code == 0x0F || (code >= 0x1D && code <= 0x1F) || (code >= 0x2D && code <= 0x2F) ||
        (code >= 0x3C && code <= 0x3F) || code == 0x43 || code == 0x4A || (code >= 0x5A && code <= 0x5E))
        q |= IEQUALIFIER_NUMERICPAD;
    return q;
}

/* While the engine's screens are shown, every key goes to its input handler (as a raw key), none to the BIOS. */
static bool key_hook(int scancode, int mod, bool down, bool repeat)
{
    u8 code = amiga_raw_key((SDL_Scancode)scancode);
    if (code != 0xFF && key_handler) {
        u16 q = qualifier((SDL_Keymod)mod, code);
        if (repeat) q |= IEQUALIFIER_REPEAT;
        key_handler(down ? code : (u8)(code | 0x80), q);
    }
    return true;
}

void ahost_joystick(bool *up, bool *down, bool *left, bool *right, bool *fire)
{
    const bool *ks = SDL_GetKeyboardState(NULL);
    bool u = ks[SDL_SCANCODE_UP] || ks[SDL_SCANCODE_KP_8] || ks[SDL_SCANCODE_KP_7] || ks[SDL_SCANCODE_KP_9];
    bool d = ks[SDL_SCANCODE_DOWN] || ks[SDL_SCANCODE_KP_2] || ks[SDL_SCANCODE_KP_1] || ks[SDL_SCANCODE_KP_3];
    bool l = ks[SDL_SCANCODE_LEFT] || ks[SDL_SCANCODE_KP_4] || ks[SDL_SCANCODE_KP_7] || ks[SDL_SCANCODE_KP_1];
    bool r = ks[SDL_SCANCODE_RIGHT] || ks[SDL_SCANCODE_KP_6] || ks[SDL_SCANCODE_KP_9] || ks[SDL_SCANCODE_KP_3];
    /* Enter is fire too: the DOS game's menus take it, and players coming from them press it. */
    bool f = ks[SDL_SCANCODE_SPACE] || ks[SDL_SCANCODE_RCTRL] || ks[SDL_SCANCODE_KP_0] || ks[SDL_SCANCODE_RETURN]
             || ks[SDL_SCANCODE_KP_ENTER];
    s16 ax, ay;
    u8 buttons;
    if (host_joy_read(&ax, &ay, &buttons)) {
        const s16 dead = 12000;
        u |= ay < -dead;
        d |= ay > dead;
        l |= ax < -dead;
        r |= ax > dead;
        f |= buttons != 0;
    }
    /* A digital joystick cannot report both opposite directions. */
    if (u && d) u = d = false;
    if (l && r) l = r = false;
    *up = u; *down = d; *left = l; *right = r; *fire = f;
}

/* ---------------------------------------------------------------- errors */

_Noreturn void ahost_fatal(const char *fmt, ...)
{
    char msg[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    host_fatal("Amiga: %s", msg);
}
