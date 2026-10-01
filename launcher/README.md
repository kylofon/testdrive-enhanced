# Test Drive Enhanced (the launcher)

`Test Drive Enhanced.exe` starts `testdrive-enhanced` with the options chosen in its window. It is built with
[wxWidgets](https://www.wxwidgets.org/) 3.2 from the platform's own controls; the window, menus and pages follow
the Test Drive III Enhanced launcher.

## The window

* **Version:** **PC** (the DOS game, enhanced) or **Amiga** (the DOS game with the Amiga features below mixed in).
* **Game files**
  * **Folder:** the DOS game's files (default `Game` beside the launcher). Always needed.
  * **Amiga disk** (Amiga only): the `.adf`, a folder holding it, or a folder with the disk's files (default
    `Game Amiga`).
  * **Program:** `testdrive-enhanced.exe` (default: beside the launcher).
  * The line below says what was found or what is missing. **Play** stays greyed out until the files are there.
* **Options:** **Window size** (`--scale`), **Start in full screen** (`--fullscreen`; Alt+Enter switches) and
  **Original keyboard handling** (`--bios-keys`).
* **Amiga features** (Amiga only, `--amiga`): **All Amiga features** checks or clears all of them (it shows a mixed
  state when only some are checked), and any mix can be played: **Amiga intro**, **menu**, **dashboards**,
  **music**, **projection**, **sky**, **cliffs**, **traffic speed**, **cops**, **wide roads** and **road signs**.
  Point at one for what it does.
* **Play** starts the game; the launcher stays open.

## Menus

* **File:** **Preferences** (**Always on top** keeps the launcher above other windows) and **Exit**.
* **Game settings > Graphics** is shown in the window in place of the main page: **Resolution** (`--res-scale`,
  320 x 200 to 2560 x 1600, default 1280 x 800), **Sprite detail** (`--sprite-detail`: maximum or by distance) and
  **Frame rate** (`--frame-rate`, default 60, 0 = as fast as possible). **Default** puts them back, **Apply** keeps
  them and goes back, **Cancel** goes back without changing them.
* **Game settings > Key Bindings**: the driving and game keys, in two groups. Click a key and press the new one
  ("Press new key"; Shift, Ctrl or Alt can be held with it, Esc cancels, **No key** leaves the action without
  one). Keys are the keyboard's places, whatever its layout. A key already used by another action can be moved,
  leaving that one without a key. **Default**, **Apply** and **Cancel** as above. The game gets the changed keys
  with `--keys` and applies them while driving; the menus and the name entry keep the game's own keys. The keypad,
  Home / PgUp / End / PgDn, Esc, Enter and Alt+Enter keep their jobs.
* **About:** version, author and links.

Everything is remembered in `%APPDATA%\Test Drive Enhanced\settings.ini` (`~/.config/test-drive-enhanced` on
Linux). A folder or program left at its default is stored empty, so it follows the launcher if the whole folder
moves. Delete the file to go back to the defaults.

## Building

Needs CMake 3.24, a C++17 compiler and wxWidgets 3.2 (MSYS2 `mingw64`: `mingw-w64-x86_64-wxwidgets3.2-msw`;
Debian and Ubuntu: `libwxgtk3.2-dev`). The main build makes it beside `testdrive-enhanced.exe` when wxWidgets is
found (`-DTDE_LAUNCHER=OFF` leaves it out). It also builds on its own (`cmake -S launcher -B launcher/build -G
Ninja`); then choose the program in the window or copy the launcher next to it.

On Windows the build copies every DLL the launcher needs beside it (`copy_dlls.cmake`): the two wxWidgets DLLs and
the MSYS2 libraries they load. Keep them with the `.exe` in a release. The C++ runtime of the launcher itself is
linked in.

## Files

* `app.cpp`: the wxWidgets application.
* `launcher.h`, `launcher.cpp`: the window, its menus, the Graphics and Key Bindings pages, Preferences and About.
* `game.h`, `game.cpp`: the file checks, the Amiga features' list and starting the game.
* `keys.h`, `keys.cpp`: the keys that can be changed, their names and the "Press new key" prompt (the game's own
  list is `src/keybind.c`).
* `settings.h`, `settings.cpp`: `settings.ini`.
* `icon.h`, `icon.cpp`: the app icon, a chequered flag drawn in code. `make_icon.py` (Pillow) writes the same
  drawing to `app.ico` for Explorer.
* `app.rc`, `app.manifest`, `app.ico`, `version.h`: icon, visual styles, DPI awareness, version info.
* `copy_dlls.cmake`: the post-build DLL copy.
* `CMakeLists.txt`: the build, standalone or from the main project.
