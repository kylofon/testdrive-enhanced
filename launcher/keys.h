// keys.h -- the game's keys that can be changed (Game settings > Key Bindings), kept in settings.ini and
// handed to the port with --keys.
//
// A key is a PC keyboard's place: its XT make code, GREY for the keys sent with an E0 prefix (arrows, Home,
// Right Ctrl...), with the modifiers held with it. The game's list (src/keybind.c) has the same names and
// defaults.
#pragma once

#include <wx/string.h>

#include <vector>

class wxWindow;

namespace keys {

enum : int { GREY = 0x100, SHIFT = 0x1000, CTRL = 0x2000, ALT = 0x4000 };

enum Group { DRIVING, GAME, GROUP_COUNT };

struct Action {
    const char* name;   // in settings.ini and on the port's command line
    const char* label;
    int def;            // the default key
    Group group;
};

extern const Action ACTIONS[];
extern const int ACTION_COUNT;
extern const char* const GROUP_TITLES[GROUP_COUNT];

// Keys that keep their job and can't be changed: {keys, what they do}.
struct FixedKey {
    const char* keys;
    const char* what;
};
extern const FixedKey FIXED[];
extern const int FIXED_COUNT;

// The key of each action, in the order of ACTIONS; 0 = none.
using Bindings = std::vector<int>;

Bindings Defaults();
Bindings Load();
void Save(const Bindings& bindings);

// "Shift+F", "Up"; "(none)" for 0.
wxString Name(int key);

// The --keys argument: the actions not on their default keys, "name=0x21,..."; empty if none.
wxString Argument(const Bindings& bindings);

// Asks for a new key for `action`. Returns the key, 0 for "no key", or -1 if cancelled.
int AskKey(wxWindow* parent, const wxString& action);

}  // namespace keys
