// game.h -- where the game's files and testdrive-enhanced are, the file checks and starting the game.
#pragma once

#include <wx/string.h>

// The folder the launcher runs from.
wxString LauncherDir();

// Where things are by default, beside the launcher: "Game" (the DOS files), "Game Amiga" (the Amiga disk) and
// testdrive-enhanced(.exe).
wxString DefaultGameDir();
wxString DefaultAmigaDisk();
wxString DefaultProgram();

// The first of the DOS game's own files missing from `dir`, or empty if they are all there.
wxString MissingGameFile(const wxString& dir);

// What the Amiga disk `path` holds: an .adf file, a folder with an .adf in it, or a folder with the files of
// the disk (it has td). Returns the name found (the .adf or "td"), or empty if there is none.
wxString FindAmigaDisk(const wxString& path);

// The Amiga features (testdrive-enhanced --amiga), in the order of the launcher's checkboxes.
struct AmigaFeature {
    const char* name;      // on the command line
    const char* setting;   // in settings.ini
    const char* label;
    const char* tip;
};
extern const AmigaFeature AMIGA_FEATURES[];
extern const int AMIGA_FEATURE_COUNT;

struct GameOptions {
    wxString program;       // testdrive-enhanced(.exe)
    wxString gameDir;       // --game-dir
    int scale = 3;          // --scale: the window is 320x240 times this
    bool fullscreen = false;  // --fullscreen
    bool biosKeys = false;  // --bios-keys
    int resScale = 4;       // --res-scale: the picture is 320x200 times this
    bool spriteMax = true;  // --sprite-detail max (else auto)
    int frameRate = 60;     // --frame-rate: frames a second while driving (0 = as fast as possible)
    wxString keys;          // --keys: the keys not on their defaults (keys::Argument)
    wxString amiga;         // --amiga: the features chosen, comma separated; empty = the PC version
    wxString amigaDisk;     // --amiga-disk
};

// Starts the game. On failure returns false and says why in `error`.
bool LaunchGame(const GameOptions& options, wxString& error);
