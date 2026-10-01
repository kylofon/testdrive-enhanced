/* Key bindings: see keybind.h. */
#include "keybind.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GREY  KB_GREY
#define CTRL  KB_CTRL
#define MODS  (KB_SHIFT | KB_CTRL | KB_ALT)
#define BOTH  (KB_DOS | KB_AMIGA)

/* The launcher's list (launcher/keys.cpp): the same names and defaults, in the order of KbAction. */
static const struct {
    const char *name;
    u16 def;            /* the key it has by default */
    u8 versions;        /* KB_DOS, KB_AMIGA */
} actions[KB_COUNT] = {
    { "accelerate",  GREY | 0x48, BOTH },
    { "brake",       GREY | 0x50, BOTH },
    { "steer_left",  GREY | 0x4B, BOTH },
    { "steer_right", GREY | 0x4D, BOTH },
    { "shift_up",    0x1E,        KB_DOS },
    { "shift_down",  0x2C,        KB_DOS },
    { "fire",        0x39,        KB_AMIGA },
    { "pause",       0x19,        BOTH },
    { "gearbox",     0x20,        BOTH },
    { "gate",        0x18,        BOTH },
    { "sound_off",   CTRL | 0x10, KB_DOS },
    { "sound_on",    CTRL | 0x1F, KB_DOS },
    { "joystick",    CTRL | 0x24, KB_DOS },
    { "keyboard",    CTRL | 0x25, KB_DOS },
    { "music",       0x32,        KB_AMIGA },
    { "sound",       0x1F,        KB_AMIGA },
};

static u16 keys[KB_COUNT];                 /* the key each action has now (0 = none) */
static SDL_Scancode scs[KB_COUNT];         /* ... its scancode */
static SDL_Scancode def_scs[KB_COUNT];     /* the scancode of its default key */
static int version = BOTH;
static bool active;

u16 kb_key(SDL_Scancode sc)
{
    if (sc >= SDL_SCANCODE_A && sc <= SDL_SCANCODE_Z) {
        static const u8 letter_scan[26] = { 0x1E,0x30,0x2E,0x20,0x12,0x21,0x22,0x23,0x17,0x24,0x25,0x26,0x32,
                                            0x31,0x18,0x19,0x10,0x13,0x1F,0x14,0x16,0x2F,0x11,0x2D,0x15,0x2C };
        return letter_scan[sc - SDL_SCANCODE_A];
    }
    if (sc >= SDL_SCANCODE_1 && sc <= SDL_SCANCODE_0) return (u16)(0x02 + (sc - SDL_SCANCODE_1));
    if (sc >= SDL_SCANCODE_F1 && sc <= SDL_SCANCODE_F10) return (u16)(0x3B + (sc - SDL_SCANCODE_F1));
    switch (sc) {
    case SDL_SCANCODE_F11:          return 0x57;
    case SDL_SCANCODE_F12:          return 0x58;
    case SDL_SCANCODE_ESCAPE:       return 0x01;
    case SDL_SCANCODE_MINUS:        return 0x0C;
    case SDL_SCANCODE_EQUALS:       return 0x0D;
    case SDL_SCANCODE_BACKSPACE:    return 0x0E;
    case SDL_SCANCODE_TAB:          return 0x0F;
    case SDL_SCANCODE_LEFTBRACKET:  return 0x1A;
    case SDL_SCANCODE_RIGHTBRACKET: return 0x1B;
    case SDL_SCANCODE_RETURN:       return 0x1C;
    case SDL_SCANCODE_KP_ENTER:     return GREY | 0x1C;
    case SDL_SCANCODE_LCTRL:        return 0x1D;
    case SDL_SCANCODE_RCTRL:        return GREY | 0x1D;
    case SDL_SCANCODE_SEMICOLON:    return 0x27;
    case SDL_SCANCODE_APOSTROPHE:   return 0x28;
    case SDL_SCANCODE_GRAVE:        return 0x29;
    case SDL_SCANCODE_LSHIFT:       return 0x2A;
    case SDL_SCANCODE_BACKSLASH:    return 0x2B;
    case SDL_SCANCODE_COMMA:        return 0x33;
    case SDL_SCANCODE_PERIOD:       return 0x34;
    case SDL_SCANCODE_SLASH:        return 0x35;
    case SDL_SCANCODE_KP_DIVIDE:    return GREY | 0x35;
    case SDL_SCANCODE_RSHIFT:       return 0x36;
    case SDL_SCANCODE_KP_MULTIPLY:  return 0x37;
    case SDL_SCANCODE_LALT:         return 0x38;
    case SDL_SCANCODE_RALT:         return GREY | 0x38;
    case SDL_SCANCODE_SPACE:        return 0x39;
    case SDL_SCANCODE_CAPSLOCK:     return 0x3A;
    case SDL_SCANCODE_NUMLOCKCLEAR: return 0x45;
    case SDL_SCANCODE_SCROLLLOCK:   return 0x46;
    case SDL_SCANCODE_KP_7:         return 0x47;
    case SDL_SCANCODE_KP_8:         return 0x48;
    case SDL_SCANCODE_KP_9:         return 0x49;
    case SDL_SCANCODE_KP_MINUS:     return 0x4A;
    case SDL_SCANCODE_KP_4:         return 0x4B;
    case SDL_SCANCODE_KP_5:         return 0x4C;
    case SDL_SCANCODE_KP_6:         return 0x4D;
    case SDL_SCANCODE_KP_PLUS:      return 0x4E;
    case SDL_SCANCODE_KP_1:         return 0x4F;
    case SDL_SCANCODE_KP_2:         return 0x50;
    case SDL_SCANCODE_KP_3:         return 0x51;
    case SDL_SCANCODE_KP_0:         return 0x52;
    case SDL_SCANCODE_KP_PERIOD:    return 0x53;
    case SDL_SCANCODE_HOME:         return GREY | 0x47;
    case SDL_SCANCODE_UP:           return GREY | 0x48;
    case SDL_SCANCODE_PAGEUP:       return GREY | 0x49;
    case SDL_SCANCODE_LEFT:         return GREY | 0x4B;
    case SDL_SCANCODE_RIGHT:        return GREY | 0x4D;
    case SDL_SCANCODE_END:          return GREY | 0x4F;
    case SDL_SCANCODE_DOWN:         return GREY | 0x50;
    case SDL_SCANCODE_PAGEDOWN:     return GREY | 0x51;
    case SDL_SCANCODE_INSERT:       return GREY | 0x52;
    case SDL_SCANCODE_DELETE:       return GREY | 0x53;
    default:                        return 0;
    }
}

u16 kb_mods(SDL_Keymod mod)
{
    return (u16)(((mod & SDL_KMOD_SHIFT) ? KB_SHIFT : 0) | ((mod & SDL_KMOD_CTRL) ? KB_CTRL : 0) |
                 ((mod & SDL_KMOD_ALT) ? KB_ALT : 0));
}

static SDL_Scancode scancode_of(u16 key)
{
    key &= GREY | 0x7F;
    if (!key) return SDL_SCANCODE_UNKNOWN;
    for (int sc = 1; sc < SDL_SCANCODE_COUNT; sc++)
        if (kb_key((SDL_Scancode)sc) == key) return (SDL_Scancode)sc;
    return SDL_SCANCODE_UNKNOWN;
}

static void bind(int i, u16 key)
{
    keys[i] = key;
    scs[i] = scancode_of(key);
}

static void init_once(void)
{
    static bool done;
    if (done) return;
    done = true;
    for (int i = 0; i < KB_COUNT; i++) {
        bind(i, actions[i].def);
        def_scs[i] = scs[i];
    }
}

void kb_init(int v)
{
    init_once();
    version = v;
}

bool kb_parse(const char *spec)
{
    init_once();
    char buf[1024];
    snprintf(buf, sizeof buf, "%s", spec);
    for (char *item = strtok(buf, ","); item; item = strtok(NULL, ",")) {
        char *eq = strchr(item, '=');
        if (!eq) return false;
        *eq = 0;
        char *end;
        unsigned long code = strtoul(eq + 1, &end, 0);
        if (*end || (code & ~(unsigned long)(MODS | GREY | 0x7F))) return false;
        int i = 0;
        while (i < KB_COUNT && strcmp(actions[i].name, item)) i++;
        if (i == KB_COUNT) return false;
        bind(i, (u16)code);
    }
    return true;
}

void kb_set_active(bool on) { active = on; }
bool kb_active(void) { return active; }

static bool ours(int i) { return (actions[i].versions & version) != 0; }

int kb_lookup(u16 key, bool *drop)
{
    init_once();
    *drop = false;
    if (!(key & (GREY | 0x7F))) return -1;      /* a key the game has no code for: never an action without one */
    for (int i = 0; i < KB_COUNT; i++)
        if (ours(i) && keys[i] == key) return i;
    for (int i = 0; i < KB_COUNT; i++)
        if (ours(i) && actions[i].def == key && keys[i] != key) {
            *drop = true;                       /* its action moved to another key */
            return -1;
        }
    return -1;
}

bool kb_held(KbAction a)
{
    init_once();
    const u16 key = active ? keys[a] : actions[a].def;
    const SDL_Scancode sc = active ? scs[a] : def_scs[a];
    if (sc == SDL_SCANCODE_UNKNOWN || !SDL_GetKeyboardState(NULL)[sc]) return false;
    return (kb_mods(SDL_GetModState()) & key & MODS) == (key & MODS);
}
