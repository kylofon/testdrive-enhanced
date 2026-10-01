#pragma once
/* Key bindings (the launcher's Game settings > Key Bindings, handed over with --keys), applied by host.c.
 * PORT: the original has no key bindings. Kept in step with test-drive-sdl3's tdport/src/keybind.c.
 *
 * A key is a PC keyboard's place: its XT set-1 make code, KB_GREY for the keys sent after an E0 prefix
 * (arrows, Home, Right Ctrl...), with the modifiers held with it (KB_SHIFT / CTRL / ALT). The launcher's list
 * (launcher/keys.cpp) has the same names and defaults. The bindings apply only while driving (kb_set_active);
 * the menus, the name entry and the waits for a key keep the game's own keys. */
#include <SDL3/SDL.h>

#include "types.h"

#define KB_GREY  0x0100
#define KB_SHIFT 0x1000
#define KB_CTRL  0x2000
#define KB_ALT   0x4000

/* The actions. Some belong to the DOS versions only, some to the Amiga version only (kb_init). */
typedef enum {
    KB_ACCELERATE, KB_BRAKE, KB_STEER_LEFT, KB_STEER_RIGHT,
    KB_SHIFT_UP, KB_SHIFT_DOWN,                 /* DOS: A, Z */
    KB_FIRE,                                    /* Amiga: the joystick's fire button (Space) */
    KB_PAUSE, KB_GEARBOX, KB_GATE,
    KB_SOUND_OFF, KB_SOUND_ON, KB_JOYSTICK, KB_KEYBOARD,   /* DOS: Ctrl+Q, Ctrl+S, Ctrl+J, Ctrl+K */
    KB_MUSIC, KB_SOUND,                         /* Amiga: M, S */
    KB_COUNT
} KbAction;

enum { KB_DOS = 1, KB_AMIGA = 2 };

/* The version this program is (KB_DOS or KB_AMIGA): only its actions are bound. */
void kb_init(int version);
/* --keys "name=code,...": the actions not on their default keys (code 0 = no key). False if the list can't
 * be read. Names of the other version's actions are accepted and ignored. */
bool kb_parse(const char *spec);
/* The bindings apply while driving. */
void kb_set_active(bool on);
bool kb_active(void);

/* The key of an SDL scancode (without modifiers), 0 if it has none. */
u16 kb_key(SDL_Scancode sc);
/* The modifiers of SDL's modifier state. */
u16 kb_mods(SDL_Keymod mod);

/* What a key going down does while the bindings apply: the action bound to `key` (with its modifiers), or -1.
 * *drop is set when the key is no action's but is the default key of an action moved elsewhere, so it must
 * not reach the game. */
int kb_lookup(u16 key, bool *drop);
/* True while the key of `action` is held (with its modifiers; others may be held too). While the bindings
 * don't apply, the default key. */
bool kb_held(KbAction action);
