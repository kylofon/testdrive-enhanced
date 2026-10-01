#include "host.h"

#include <SDL3/SDL.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "keybind.h"

#define AUDIO_RATE 44100
#define AUDIO_AMPLITUDE 5000

static SDL_Window *window;
static SDL_Renderer *renderer;
static SDL_Texture *texture;
static SDL_AudioStream *audio;
static SDL_Gamepad *gamepad;
static char *game_dir;

static void (*tick_handler)(void);
static bool (*frame_source)(u32 *);
static u32 *frame;
static int frame_w, frame_h;

/* Tick clock: tick n is due at start + n * 11927 / 1193182 s (exact rational arithmetic). */
static Uint64 clock_start_ns;
static Uint64 ticks_run;

/* BIOS keyboard buffer (15 keys, like the real one). */
#define KBD_SIZE 16
static u16 kbd_buf[KBD_SIZE];
static int kbd_head, kbd_tail;

/* Speaker state and square-wave generator. */
static u16 spk_div;
static bool spk_on;
static double spk_phase;
static double samples_per_tick_frac;

static void process_events(void);
static void pool_shutdown(void);

bool host_init(const char *dir, int window_scale)
{
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMEPAD)) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return false;
    }
    game_dir = SDL_strdup(dir);
    if (window_scale < 1) window_scale = 3;
    if (!SDL_CreateWindowAndRenderer("Test Drive Enhanced", 320 * window_scale, 240 * window_scale,
                                     SDL_WINDOW_RESIZABLE, &window, &renderer)) {
        fprintf(stderr, "window/renderer failed: %s\n", SDL_GetError());
        return false;
    }
    SDL_SetRenderVSync(renderer, 1);
    /* The frame texture is created by host_set_frame_source() once its size is known. */

    SDL_AudioSpec spec = { SDL_AUDIO_S16, 1, AUDIO_RATE };
    audio = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, NULL, NULL);
    if (audio) {
        SDL_ResumeAudioStreamDevice(audio);
        static s16 silence[AUDIO_RATE / 20];               /* 50 ms of lead-in against underruns */
        SDL_PutAudioStreamData(audio, silence, sizeof silence);
    } else {
        fprintf(stderr, "audio unavailable: %s\n", SDL_GetError());
    }

    clock_start_ns = SDL_GetTicksNS();
    ticks_run = 0;
    return true;
}

void host_shutdown(void)
{
    pool_shutdown();
    if (gamepad) SDL_CloseGamepad(gamepad);
    if (audio) SDL_DestroyAudioStream(audio);
    if (texture) SDL_DestroyTexture(texture);
    SDL_free(frame);
    if (renderer) SDL_DestroyRenderer(renderer);
    if (window) SDL_DestroyWindow(window);
    SDL_free(game_dir);
    SDL_Quit();
}

void host_set_fullscreen(bool on) { SDL_SetWindowFullscreen(window, on); }

void host_set_tick_handler(void (*handler)(void)) { tick_handler = handler; }

static bool frame_sharp;
static struct { bool (*compose)(u32 *); int w, h; bool sharp; } saved_source;

void host_set_frame_source(bool (*compose)(u32 *), int w, int h)
{
    frame_source = compose;
    if (w == frame_w && h == frame_h) return;
    SDL_free(frame);
    frame = SDL_calloc((size_t)w * (size_t)h, sizeof *frame);
    frame_w = w;
    frame_h = h;
    if (texture) SDL_DestroyTexture(texture);
    texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_XRGB8888, SDL_TEXTUREACCESS_STREAMING, w, h);
    /* Shown with 4:3 aspect: 320x200 (times the output scale) as on a 200-line EGA monitor, and the Amiga
     * engine's 640x200 (lowres pixels doubled) as on its monitor. */
    SDL_SetRenderLogicalPresentation(renderer, w, w * 3 / 4, SDL_LOGICAL_PRESENTATION_LETTERBOX);
    host_set_frame_sharp(frame_sharp);
}

void host_set_frame_sharp(bool on)
{
    frame_sharp = on;
    if (!texture) return;
#if SDL_VERSION_ATLEAST(3, 4, 0)
    if (on && SDL_SetTextureScaleMode(texture, SDL_SCALEMODE_PIXELART)) return;
#endif
    SDL_SetTextureScaleMode(texture, on || frame_w <= 320 ? SDL_SCALEMODE_NEAREST : SDL_SCALEMODE_LINEAR);
}

void host_save_frame_source(void)
{
    saved_source.compose = frame_source;
    saved_source.w = frame_w;
    saved_source.h = frame_h;
    saved_source.sharp = frame_sharp;
}

void host_restore_frame_source(void)
{
    if (!saved_source.compose) return;
    frame_sharp = saved_source.sharp;
    host_set_frame_source(saved_source.compose, saved_source.w, saved_source.h);
    host_set_frame_sharp(saved_source.sharp);
}

static bool (*key_hook)(int, int, bool, bool);
static void (*pump_hook)(void);
void host_set_key_hook(bool (*hook)(int scancode, int mod, bool down, bool repeat)) { key_hook = hook; }
void host_set_pump_hook(void (*hook)(void)) { pump_hook = hook; }

static Uint64 tick_due_ns(Uint64 n)
{
    return clock_start_ns + n * PIT_DIV_GAME * SDL_NS_PER_SECOND / PIT_HZ;
}

static void audio_for_one_tick(void)
{
    if (!audio) return;
    samples_per_tick_frac += (double)AUDIO_RATE * PIT_DIV_GAME / PIT_HZ;
    int n = (int)samples_per_tick_frac;
    samples_per_tick_frac -= n;
    /* Drop output if the device is far behind (e.g. after a stall) instead of building latency. */
    if (SDL_GetAudioStreamQueued(audio) > AUDIO_RATE / 4 * (int)sizeof(s16)) return;
    s16 buf[1024];
    if (n > (int)SDL_arraysize(buf)) n = SDL_arraysize(buf);
    double freq = (double)PIT_HZ / (spk_div ? spk_div : 65536);
    double step = freq / AUDIO_RATE;
    for (int i = 0; i < n; i++) {
        if (spk_on) {
            buf[i] = spk_phase < 0.5 ? AUDIO_AMPLITUDE : -AUDIO_AMPLITUDE;
            spk_phase += step;
            spk_phase -= (int)spk_phase;
        } else {
            buf[i] = 0;
        }
    }
    SDL_PutAudioStreamData(audio, buf, n * (int)sizeof(s16));
}

/* Developer aid: TD_ENH_SHOT=<folder> saves the shown frame there once a second (shot0000.bmp, ...). */
static void shot(void)
{
    static const char *dir;
    static bool checked;
    static Uint64 next_ns;
    static int n;
    if (!checked) { dir = SDL_getenv("TD_ENH_SHOT"); checked = true; }
    Uint64 now = SDL_GetTicksNS();
    if (!dir || now < next_ns) return;
    next_ns = now + SDL_NS_PER_SECOND;
    SDL_Surface *s = SDL_CreateSurfaceFrom(frame_w, frame_h, SDL_PIXELFORMAT_XRGB8888, frame, frame_w * 4);
    if (!s) return;
    char path[1024];
    SDL_snprintf(path, sizeof path, "%s/shot%04d.bmp", dir, n++);
    SDL_SaveBMP(s, path);
    SDL_DestroySurface(s);
}

static void present(void)
{
    if (!texture) return;
    shot();
    SDL_UpdateTexture(texture, NULL, frame, frame_w * 4);
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
    SDL_RenderClear(renderer);
    SDL_FRect dst = { 0, 0, (float)frame_w, (float)(frame_w * 3 / 4) };
    SDL_RenderTexture(renderer, texture, NULL, &dst);
    SDL_RenderPresent(renderer);
}

static Uint64 last_present_ns;

void host_present_now(void)
{
    if (frame_source && frame_source(frame)) {
        present();
        last_present_ns = SDL_GetTicksNS();
    }
}

void host_pump(void)
{
    process_events();
    if (pump_hook) pump_hook();

    bool worked = false;
    Uint64 now = SDL_GetTicksNS();
    int budget = 50;                                /* at most 0.5 s of catch-up per call */
    while (tick_due_ns(ticks_run + 1) <= now && budget-- > 0) {
        ticks_run++;
        if (tick_handler) tick_handler();
        audio_for_one_tick();
        worked = true;
    }
    if (budget < 0) {                               /* fell too far behind: resynchronise the clock */
        clock_start_ns = now - (tick_due_ns(ticks_run) - clock_start_ns);
    }

    /* Present at most once per ~8 ms; VSync paces it further. */
    if (frame_source && now - last_present_ns >= 8 * SDL_NS_PER_MS) {
        if (frame_source(frame)) {
            present();
            last_present_ns = SDL_GetTicksNS();
            worked = true;
        }
    }
    if (!worked) {
        Uint64 next = tick_due_ns(ticks_run + 1);
        now = SDL_GetTicksNS();
        if (next > now) SDL_DelayNS(SDL_min(next - now, SDL_NS_PER_MS));
    }
}

static int frame_rate = 8;          /* ~1987 PC AT + EGA */
static Uint64 next_frame_ns;

uint64_t host_time_ns(void) { return SDL_GetTicksNS(); }

/* ---------------------------------------------------------------- worker pool */

#define POOL_MAX 15
static SDL_Thread *pool_threads[POOL_MAX];
static int pool_size = -1;                          /* -1 = not started */
static SDL_Semaphore *pool_wake, *pool_done;
static SDL_AtomicInt pool_next;
static int pool_count;
static void (*pool_fn)(int, void *);
static void *pool_ctx;
static bool pool_quit;

static void pool_run(void)
{
    for (;;) {
        int i = SDL_AddAtomicInt(&pool_next, 1);     /* returns the previous value */
        if (i >= pool_count) return;
        pool_fn(i, pool_ctx);
    }
}

static int pool_worker(void *unused)
{
    (void)unused;
    for (;;) {
        SDL_WaitSemaphore(pool_wake);
        if (pool_quit) return 0;
        pool_run();
        SDL_SignalSemaphore(pool_done);
    }
}

static void pool_start(void)
{
    int n = SDL_GetNumLogicalCPUCores() - 1;
    if (n > POOL_MAX) n = POOL_MAX;
    pool_size = 0;
    if (n <= 0) return;
    pool_wake = SDL_CreateSemaphore(0);
    pool_done = SDL_CreateSemaphore(0);
    if (!pool_wake || !pool_done) return;
    for (int i = 0; i < n; i++) {
        pool_threads[i] = SDL_CreateThread(pool_worker, "pool", NULL);
        if (!pool_threads[i]) break;
        pool_size++;
    }
}

static void pool_shutdown(void)
{
    if (pool_size <= 0) return;
    pool_quit = true;
    for (int i = 0; i < pool_size; i++) SDL_SignalSemaphore(pool_wake);
    for (int i = 0; i < pool_size; i++) SDL_WaitThread(pool_threads[i], NULL);
    SDL_DestroySemaphore(pool_wake);
    SDL_DestroySemaphore(pool_done);
    pool_size = 0;
}

void host_parallel_for(int n, void (*fn)(int i, void *ctx), void *ctx)
{
    if (pool_size < 0) pool_start();
    pool_fn = fn;
    pool_ctx = ctx;
    pool_count = n;
    SDL_SetAtomicInt(&pool_next, 0);
    int helpers = pool_size < n - 1 ? pool_size : n - 1;
    for (int i = 0; i < helpers; i++) SDL_SignalSemaphore(pool_wake);
    pool_run();
    for (int i = 0; i < helpers; i++) SDL_WaitSemaphore(pool_done);
}

void host_set_frame_rate(int fps) { frame_rate = fps < 0 ? 0 : fps; }
int  host_frame_rate(void) { return frame_rate; }

void host_frame_begin(void)
{
    host_pump();
    if (frame_rate <= 0) return;
    Uint64 period = SDL_NS_PER_SECOND / (Uint64)frame_rate;
    Uint64 now = SDL_GetTicksNS();
    if (next_frame_ns == 0 || now > next_frame_ns + period) next_frame_ns = now;   /* resync after stalls */
    while (SDL_GetTicksNS() < next_frame_ns) host_pump();
    next_frame_ns += period;
}

/* ---------------------------------------------------------------- keyboard */

static void kbd_push(u16 key)
{
    int next = (kbd_tail + 1) % KBD_SIZE;
    if (next == kbd_head) return;                   /* buffer full: BIOS beeps and drops the key */
    kbd_buf[kbd_tail] = key;
    kbd_tail = next;
}

bool host_kbd_peek(u16 *key)
{
    process_events();
    if (kbd_head == kbd_tail) return false;
    if (key) *key = kbd_buf[kbd_head];
    return true;
}

bool host_kbd_read(u16 *key)
{
    process_events();
    if (kbd_head == kbd_tail) return false;
    if (key) *key = kbd_buf[kbd_head];
    kbd_head = (kbd_head + 1) % KBD_SIZE;
    return true;
}

void host_kbd_flush(void)
{
    process_events();
    kbd_head = kbd_tail = 0;
}

u8 host_kbd_shift_flags(void)
{
    SDL_Keymod m = SDL_GetModState();
    u8 f = 0;
    if (m & SDL_KMOD_RSHIFT) f |= 0x01;
    if (m & SDL_KMOD_LSHIFT) f |= 0x02;
    if (m & SDL_KMOD_CTRL)   f |= 0x04;
    if (m & SDL_KMOD_ALT)    f |= 0x08;
    if (m & SDL_KMOD_NUM)    f |= 0x20;
    if (m & SDL_KMOD_CAPS)   f |= 0x40;
    return f;
}

static bool held_keys = true;

void host_set_held_keys(bool on) { held_keys = on; }
bool host_held_keys(void) { return held_keys; }

bool host_xt_key_down(u8 xt)
{
    const bool *ks = SDL_GetKeyboardState(NULL);
    /* Keypad digits are cursor keys only with Num Lock off; with it on they type digits, which the
     * game maps to fire + direction through the key buffer. */
    bool num = (SDL_GetModState() & SDL_KMOD_NUM) != 0;
#define KP(sc) (!num && ks[sc])
    /* PORT: A, Z and the arrows are the keys of their actions (keybind.h); Home, PgUp, End, PgDn and the keypad
     * keep their job. */
    switch (xt) {
    case 0x1E: return kb_held(KB_SHIFT_UP);
    case 0x2C: return kb_held(KB_SHIFT_DOWN);
    case 0x47: return ks[SDL_SCANCODE_HOME]     || KP(SDL_SCANCODE_KP_7);
    case 0x48: return kb_held(KB_ACCELERATE)    || KP(SDL_SCANCODE_KP_8);
    case 0x49: return ks[SDL_SCANCODE_PAGEUP]   || KP(SDL_SCANCODE_KP_9);
    case 0x4B: return kb_held(KB_STEER_LEFT)    || KP(SDL_SCANCODE_KP_4);
    case 0x4D: return kb_held(KB_STEER_RIGHT)   || KP(SDL_SCANCODE_KP_6);
    case 0x4F: return ks[SDL_SCANCODE_END]      || KP(SDL_SCANCODE_KP_1);
    case 0x50: return kb_held(KB_BRAKE)         || KP(SDL_SCANCODE_KP_2);
    case 0x51: return ks[SDL_SCANCODE_PAGEDOWN] || KP(SDL_SCANCODE_KP_3);
#undef KP
    default:   return false;
    }
}

/* XT scan codes (set 1) and ASCII for a US keyboard, as INT 16h AH=00h reports them. */
static u16 bios_key(SDL_Keycode k, SDL_Scancode sc, SDL_Keymod mod)
{
    bool shift = (mod & SDL_KMOD_SHIFT) != 0, ctrl = (mod & SDL_KMOD_CTRL) != 0, alt = (mod & SDL_KMOD_ALT) != 0;
    bool caps = (mod & SDL_KMOD_CAPS) != 0, num = (mod & SDL_KMOD_NUM) != 0;
    static const u8 letter_scan[26] = { 0x1E,0x30,0x2E,0x20,0x12,0x21,0x22,0x23,0x17,0x24,0x25,0x26,0x32,
                                        0x31,0x18,0x19,0x10,0x13,0x1F,0x14,0x16,0x2F,0x11,0x2D,0x15,0x2C };
    if (k >= SDLK_A && k <= SDLK_Z) {
        int i = (int)(k - SDLK_A);
        u8 scan = letter_scan[i];
        if (alt) return (u16)(scan << 8);
        if (ctrl) return (u16)(scan << 8 | (i + 1));
        bool upper = shift != caps;
        return (u16)(scan << 8 | ((upper ? 'A' : 'a') + i));
    }
    static const char digits_shift[] = ")!@#$%^&*(";
    if (k >= SDLK_0 && k <= SDLK_9) {
        int d = (int)(k - SDLK_0);
        u8 scan = d == 0 ? 0x0B : (u8)(0x01 + d);
        if (alt) return (u16)((0x78 + (d == 0 ? 9 : d - 1)) << 8);
        return (u16)(scan << 8 | (u8)(shift ? digits_shift[d] : '0' + d));
    }
    /* Numeric keypad: digits with Num Lock, cursor keys without. */
    struct { SDL_Keycode kc; u8 scan; char digit; } kp[] = {
        { SDLK_KP_7, 0x47, '7' }, { SDLK_KP_8, 0x48, '8' }, { SDLK_KP_9, 0x49, '9' },
        { SDLK_KP_4, 0x4B, '4' }, { SDLK_KP_5, 0x4C, '5' }, { SDLK_KP_6, 0x4D, '6' },
        { SDLK_KP_1, 0x4F, '1' }, { SDLK_KP_2, 0x50, '2' }, { SDLK_KP_3, 0x51, '3' },
        { SDLK_KP_0, 0x52, '0' }, { SDLK_KP_PERIOD, 0x53, '.' },
    };
    for (size_t i = 0; i < SDL_arraysize(kp); i++)
        if (k == kp[i].kc) return (u16)(kp[i].scan << 8 | ((num != shift) ? (u8)kp[i].digit : (kp[i].scan == 0x4C ? 0 : 0)));
    switch (k) {
    case SDLK_ESCAPE:    return 0x011B;
    case SDLK_RETURN:
    case SDLK_KP_ENTER:  return ctrl ? 0x1C0A : 0x1C0D;
    case SDLK_BACKSPACE: return ctrl ? 0x0E7F : 0x0E08;
    case SDLK_TAB:       return shift ? 0x0F00 : 0x0F09;
    case SDLK_SPACE:     return 0x3920;
    case SDLK_UP:        return 0x4800;
    case SDLK_DOWN:      return 0x5000;
    case SDLK_LEFT:      return 0x4B00;
    case SDLK_RIGHT:     return 0x4D00;
    case SDLK_HOME:      return 0x4700;
    case SDLK_END:       return 0x4F00;
    case SDLK_PAGEUP:    return 0x4900;
    case SDLK_PAGEDOWN:  return 0x5100;
    case SDLK_INSERT:    return 0x5200;
    case SDLK_DELETE:    return 0x5300;
    case SDLK_MINUS:     return shift ? 0x0C5F : 0x0C2D;
    case SDLK_EQUALS:    return shift ? 0x0D2B : 0x0D3D;
    case SDLK_LEFTBRACKET:  return shift ? 0x1A7B : 0x1A5B;
    case SDLK_RIGHTBRACKET: return shift ? 0x1B7D : 0x1B5D;
    case SDLK_SEMICOLON: return shift ? 0x273A : 0x273B;
    case SDLK_APOSTROPHE:return shift ? 0x2822 : 0x2827;
    case SDLK_GRAVE:     return shift ? 0x297E : 0x2960;
    case SDLK_BACKSLASH: return shift ? 0x2B7C : 0x2B5C;
    case SDLK_COMMA:     return shift ? 0x333C : 0x332C;
    case SDLK_PERIOD:    return shift ? 0x343E : 0x342E;
    case SDLK_SLASH:     return shift ? 0x353F : 0x352F;
    case SDLK_KP_MINUS:  return 0x4A2D;
    case SDLK_KP_PLUS:   return 0x4E2B;
    case SDLK_KP_MULTIPLY: return 0x372A;
    default: break;
    }
    if (k >= SDLK_F1 && k <= SDLK_F10) return (u16)((0x3B + (k - SDLK_F1)) << 8);
    (void)sc;
    return 0;
}

/* The BIOS key word of each action's default key (keybind.h; 0 = not a DOS action). */
static const u16 BINDING_KEYS[KB_COUNT] = {
    [KB_ACCELERATE] = 0x4800, [KB_BRAKE] = 0x5000, [KB_STEER_LEFT] = 0x4B00, [KB_STEER_RIGHT] = 0x4D00,
    [KB_SHIFT_UP] = 0x1E61,   [KB_SHIFT_DOWN] = 0x2C7A,
    [KB_PAUSE] = 0x1970,      [KB_GEARBOX] = 0x2064,   [KB_GATE] = 0x186F,
    [KB_SOUND_OFF] = 0x1011,  [KB_SOUND_ON] = 0x1F13,  [KB_JOYSTICK] = 0x240A, [KB_KEYBOARD] = 0x250B,
};

static void process_events(void)
{
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
        switch (ev.type) {
        case SDL_EVENT_QUIT:
            host_shutdown();
            exit(0);
        case SDL_EVENT_KEY_UP:
            if (key_hook) key_hook(ev.key.scancode, ev.key.mod, false, false);
            break;
        case SDL_EVENT_KEY_DOWN: {
            /* Alt+Enter toggles fullscreen; everything else goes to the BIOS buffer, repeats included. */
            if (ev.key.key == SDLK_RETURN && (ev.key.mod & SDL_KMOD_ALT)) {
                if (!ev.key.repeat)
                    SDL_SetWindowFullscreen(window, !(SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN));
                break;
            }
            if (key_hook && key_hook(ev.key.scancode, ev.key.mod, true, ev.key.repeat)) break;
            if (kb_active()) {                      /* PORT: key bindings while driving */
                bool drop;
                int a = kb_lookup((u16)(kb_key(ev.key.scancode) | kb_mods(ev.key.mod)), &drop);
                if (a >= 0) kbd_push(BINDING_KEYS[a]);
                if (a >= 0 || drop) break;
            }
            u16 key = bios_key(ev.key.key, ev.key.scancode, ev.key.mod);
            if (key) kbd_push(key);
            break;
        }
        case SDL_EVENT_GAMEPAD_ADDED:
            if (!gamepad) gamepad = SDL_OpenGamepad(ev.gdevice.which);
            break;
        case SDL_EVENT_GAMEPAD_REMOVED:
            if (gamepad && SDL_GetGamepadID(gamepad) == ev.gdevice.which) {
                SDL_CloseGamepad(gamepad);
                gamepad = NULL;
            }
            break;
        default:
            break;
        }
    }
}

bool host_joy_read(s16 *x, s16 *y, u8 *buttons)
{
    if (!gamepad) return false;
    s16 ax = SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_LEFTX);
    s16 ay = SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_LEFTY);
    if (SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_DPAD_LEFT))  ax = -32768;
    if (SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_DPAD_RIGHT)) ax = 32767;
    if (SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_DPAD_UP))    ay = -32768;
    if (SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_DPAD_DOWN))  ay = 32767;
    u8 b = 0;
    if (SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_SOUTH)) b |= 1;
    if (SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_EAST))  b |= 2;
    if (x) *x = ax;
    if (y) *y = ay;
    if (buttons) *buttons = b;
    return true;
}

/* ---------------------------------------------------------------- speaker, files, errors */

void host_speaker(u16 divisor, bool on)
{
    spk_div = divisor;
    spk_on = on;
}

char *host_game_path(const char *name, bool create)
{
    char *direct = NULL;
    SDL_asprintf(&direct, "%s/%s", game_dir, name);
    if (SDL_GetPathInfo(direct, NULL)) return direct;

    int count = 0;
    char **entries = SDL_GlobDirectory(game_dir, NULL, 0, &count);
    char *found = NULL;
    for (int i = 0; entries && i < count; i++) {
        if (SDL_strcasecmp(entries[i], name) == 0) {
            SDL_asprintf(&found, "%s/%s", game_dir, entries[i]);
            break;
        }
    }
    SDL_free(entries);
    if (found) { SDL_free(direct); return found; }
    if (create) return direct;
    SDL_free(direct);
    return NULL;
}

void host_free(void *p)
{
    SDL_free(p);
}

_Noreturn void host_fatal(const char *fmt, ...)
{
    char msg[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    fprintf(stderr, "fatal: %s\n", msg);
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Test Drive", msg, window);
    host_shutdown();
    exit(3);
}
