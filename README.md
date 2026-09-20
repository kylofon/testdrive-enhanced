# Test Drive Enhanced — SDL3

An enhanced version of the EGA *Test Drive* (1987) by Accolade / Distinctive Software, running natively on
SDL3. The game itself is the faithful C reimplementation from
[test-drive-sdl3](https://github.com/kylofon/test-drive-sdl3), so driving, traffic, police and the game flow
work as in the original. The view through the windscreen is redrawn at a higher resolution, further draw
distance and 60 fps. It is not an emulator - the original data is not redistributed, and you need to get it yourself.

## How to play (Windows)

You need the files of the original DOS *Test Drive*. They are not included.

1. Open the [latest release](https://github.com/kylofon/testdrive-enhanced/releases/latest) and download
   `testdrive-enhanced-…-win64.zip`.
2. Put your original game files in a folder named `Game`.
3. Open the zip. Copy everything inside its `testdrive-enhanced-…-win64` folder into the folder that holds
   `Game`, so that `testdrive-enhanced.exe` sits next to `Game`:

   ```text
   Test Drive\
   ├── Game\                    <- your original game files (TDEGA.EXE, CARS.TXT, SCORES, ...)
   ├── testdrive-enhanced.exe
   ├── SDL3.dll
   ├── libiconv-2.dll
   └── (the other files from the zip)
   ```

4. Double-click `testdrive-enhanced.exe`.

Keep the folder somewhere you can save files, such as Documents or the Desktop, not Program Files. The game
saves its high scores in `Game`. If Windows says "Windows protected your PC", click **More info**, then
**Run anyway**. Press Alt+Enter for fullscreen. The keys are listed under [Controls](#controls-from-the-original).
If driving stutters on an older PC, see `--res-scale` under [Run](#run).

## Requirements

* Your game files in a folder. The game needs `TDEGA.EXE`, `CARS.TXT`, `SCORES`, `TDSND.SND`, the `*.PES`
  archives and the car `*.BIN` / `*.SS` files. By default it looks in `Game` under the working directory.
* CMake 3.24+, a C11 compiler and SDL 3.
* A multi-core CPU for the default resolution. Rendering is split across all cores.

## Build

From the repository root, in Git Bash or an MSYS2 MinGW64 shell:

```bash
export PATH="/c/msys64/mingw64/bin:$PATH"
cmake -S . -B build -G Ninja -DCMAKE_C_COMPILER=gcc -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

A `Makefile` wraps the same commands if you prefer: `make build`, `make check`, `make run`,
`make clean`, and `make syntax` to compile-check the sources without linking. `make help`
lists every target with its options. Git Bash has no `make`; use MSYS2's `mingw32-make`.

## Run

```bash
./build/testdrive-enhanced.exe --game-dir Game
```

Or `make run GAME_DIR=Game SCALE=3`.

| Option | Meaning |
|---|---|
| `--game-dir DIR` | Folder with the original game files (default `Game`) |
| `--scale N` | Initial window size as a multiple of 320×240 (default 3) |
| `--res-scale N` | Output resolution as a multiple of 320×200 (default 4 = 1280×800, range 1–8; lower it on slower CPUs) |
| `--frame-rate FPS` | Drawing rate while driving (default 60, `0` = unpaced) |
| `--sprite-detail max\|auto` | Sprite sizes: `max` (default) the most detailed sprite everywhere, `auto` chosen by distance |
| `--bios-keys` | Original keyboard behaviour for driving: keys act only through key repeat (see below) |
| `--check` | Verify `TDEGA.EXE` loads and exit, without opening a window |

Alt+Enter toggles fullscreen. The window keeps the 4:3 aspect of a 200-line EGA monitor.

## Controls (from the original)

* Arrow keys / numeric keypad: steer, accelerate, brake and shift through the gear gate, as in the original.
* Esc: quit the current drive or menu.
* Ctrl-J / Ctrl-K: joystick / keyboard control. A connected gamepad acts as the joystick (left stick or
  D-pad, A = fire).
* Ctrl-Q / Ctrl-S: sound off / on.

## Changes from original

### Road view

* **Resolution:** the road is drawn at 4× the original resolution by default, with smoothed edges. The cockpit,
  mirror and sprites keep their original pixel art, scaled up.
* **Draw distance:** 120 road rows instead of 40. The road and cliffs get lighter and hazier in the distance.
* **Smooth motion:** 60 fps instead of 8. The road, the traffic and the scenery move continuously instead of
  stepping with the simulation, and steering drift is smoothed. The rows nearest the car keep straight edges.
* **Horizon:** on the open side, a horizon below eye level with mountains above it and a valley floor below it
  that moves as you drive. A hillside falls away under the left road edge, so on left bends the far road sits
  on its own slope.
* **Cliff:** the original's plain rock face, with the slant of its cliff-edge sprite, reaching the top of the
  window. Its outline is notched, like the hillside's on the open side. The grass mounds at its foot are
  always green. The original's colour depended on what was behind them.
* **Objects:** signs, poles, traffic and the police car are scaled smoothly with distance and always use
  their largest, most detailed sprite, without sparkling in the distance. They are hidden behind hill crests
  and the cliff, and traffic that appears in the distance fades in.
* **Distance and time** in the top right, in Test Drive II's style: miles left to the end of the stage and
  the stage clock, counting the same seconds the results screen shows.
* **Kept from the original:** the mirror, dashboard, speeding ticket, "Pulling into…" messages, windscreen
  cracks and GAME OVER.

### Game (from the faithful port)

* **Held-key driving:** arrows / keypad and A / Z are read while held, not only through key repeat
  (`--bios-keys` restores the original).
* **Timing:** the gear-shift panel close delay and the windscreen crash animation keep the original 8 fps timing.
  The driving model is unchanged. For example, the brake does nothing while the tyres are skidding, as in the original.
* **Removed:** copy protection, the TD.EXE launcher password, and Hercules / CGA modes.
* **Missing SCORES:** starts with an empty table instead of exiting.
* **Extended-ASCII keys:** ignored instead of crashing.

## Layout

See `ENGINE.md` for the architecture and the rules the engine follows. In short:
* `src/enhanced/` holds the enhanced road renderer, the screen overlay and the distance and time readout.
* `src/mem.*` emulates the real-mode address space the game ran in.
* `src/host.*` wraps SDL3 and runs the render worker threads.
* `src/platform/` holds the EGA graphics, timer/sound, input and resource layers.
* `src/game/` holds game flow, scene rendering and simulation.

Reverse-engineering tools, specs and file formats are in
[test-drive-sdl3](https://github.com/kylofon/test-drive-sdl3).

## License

MIT, see [LICENSE](LICENSE). *Test Drive* and its data belong to their respective owners.

## Support

https://buymeacoffee.com/krzysztofkania
