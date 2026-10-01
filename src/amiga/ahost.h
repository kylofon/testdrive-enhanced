#pragma once
/* Host services of the Amiga port on top of SDL3: the window, the 60 Hz vertical blank (NTSC,
 * port/amiga/README.md Decisions 1) with its interrupt servers, the audio stream, raw keys, the joystick in
 * port 2 and fatal errors (the game files are adisk.h). No game logic lives here. */
#include "../types.h"

#define VBL_HZ 60                   /* NTSC vertical blank */
#define AUDIO_RATE 44100            /* host output rate; 735 stereo frames per VBL */

bool ahost_init(int window_scale);
void ahost_shutdown(void);

/* ---- The vertical blank. Each 1/60 s tick calls the servers in exec's order (higher priority first; the
 * game's are Song 0x20, Sfx 0x1E, Ticks -0x50), then renders 1/60 s of audio. The ticks run from
 * ahost_pump(), which every wait of the original (WaitTOF, Delay, key and deadline polls) must call. */
void ahost_add_vbl_server(int pri, void (*server)(void));
void ahost_remove_vbl_server(void (*server)(void));
u32  ahost_vbl_count(void);          /* ticks run so far */

/* Runs due ticks, handles window events and presents the display. Sleeps briefly when nothing was due. */
void ahost_pump(void);
/* graphics.library WaitTOF: returns after the next vertical blank. */
void ahost_wait_tof(void);
/* dos.library Delay: n/50 s of real time (timer.device, not the VBL), running the ticks that fall into it. */
void ahost_delay(u32 fiftieths);
/* Host time in nanoseconds, and pumping until a point in it (for work the original did at CPU speed). */
u64  ahost_time_ns(void);
void ahost_wait_until_ns(u64 t);

/* ---- Display: fills a w x h XRGB8888 frame (at most 640 x 256), shown with 4:3 aspect. */
#define AHOST_FRAME_MAX_W 640
#define AHOST_FRAME_MAX_H 256
void ahost_set_frame_source(void (*compose)(u32 *xrgb), int w, int h);

/* ---- Audio: called once per tick to render `frames` interleaved stereo S16 frames (left, right). */
void ahost_set_audio_source(void (*render)(s16 *lr, int frames));

/* ---- Keyboard: every key-down (repeats included, qualifier bit 0x200) and key-up (code | 0x80) as an Amiga
 * raw key code and input.device qualifier, delivered to the handler the way input.device calls its handlers. */
#define IEQUALIFIER_LSHIFT     0x0001
#define IEQUALIFIER_RSHIFT     0x0002
#define IEQUALIFIER_CAPSLOCK   0x0004
#define IEQUALIFIER_CONTROL    0x0008
#define IEQUALIFIER_LALT       0x0010
#define IEQUALIFIER_RALT       0x0020
#define IEQUALIFIER_LCOMMAND   0x0040
#define IEQUALIFIER_RCOMMAND   0x0080
#define IEQUALIFIER_NUMERICPAD 0x0100
#define IEQUALIFIER_REPEAT     0x0200
void ahost_set_key_handler(void (*handler)(u8 code, u16 qualifier));

/* ---- Joystick in port 2: the cursor keys or keypad digits and Space / Right Ctrl / keypad 0 for fire, or
 * the first gamepad (stick or d-pad, A or B). */
void ahost_joystick(bool *up, bool *down, bool *left, bool *right, bool *fire);

/* ---- Errors: shows a message box, shuts down and exits with code 3. */
_Noreturn void ahost_fatal(const char *fmt, ...);
