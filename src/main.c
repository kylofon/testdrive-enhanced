/* Test Drive Enhanced — entry point.
 *
 * usage: testdrive-enhanced [--game-dir DIR] [--scale N] [--fullscreen] [--res-scale N] [--frame-rate FPS]
 *                           [--sprite-detail max|auto] [--bios-keys] [--keys NAME=CODE,...]
 *                           [--amiga LIST] [--amiga-disk PATH] [--check]
 *   --res-scale  output resolution as a multiple of 320x200 (default 4, 1..8)
 *   --frame-rate drawing rate while driving (default 60; 0 = as fast as possible)
 *   --sprite-detail max (default): the most detailed sprite of every car and object at every distance;
 *                auto: the sprite scale chosen by distance
 *   --bios-keys driving keys act only through key repeat, exactly like the original (default: held keys)
 *   --keys      key bindings while driving, "name=code,..." for the actions not on their default keys (the
 *               launcher's Game settings > Key Bindings; keybind.c)
 *   --amiga     Amiga features, "all" or a list (amiga_opts.h)
 *   --amiga-disk the Amiga disk image (.adf), a folder holding it, or a folder with its files (default
 *               "Game Amiga"); needed by --amiga
 *   --game-dir  folder with the original game files (default: "Game" next to the working directory)
 *   --scale     initial window scale (default 3)
 *   --fullscreen start in full screen (Alt+Enter switches)
 *   --check     load and verify TDEGA.EXE, print a summary and exit (no window)
 */
#define SDL_MAIN_HANDLED
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "amiga_opts.h"
#include "host.h"
#include "keybind.h"
#include "mem.h"
#include "enhanced/enhanced.h"
#include "platform/gfx.h"

int game_main(void);   /* game/flow.c: port of main() at image 0x0010 */

static int usage(const char *prog)
{
    fprintf(stderr,
            "usage: %s [--game-dir DIR] [--scale N] [--fullscreen] [--res-scale N] [--frame-rate FPS]\n"
            "          [--sprite-detail max|auto] [--bios-keys] [--keys NAME=CODE,...]\n"
            "          [--amiga all|intro,menu,dashboards,music,projection,sky,cliffs,traffic,cops,wide-roads,signs]\n"
            "          [--amiga-disk PATH] [--check]\n", prog);
    return 2;
}

int main(int argc, char **argv)
{
    const char *dir = "Game";
    int scale = 3, res_scale = 4;
    bool check = false, rate_set = false, fullscreen = false;
    kb_init(KB_DOS);
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i], *v = i + 1 < argc ? argv[i + 1] : NULL;
        if (!strcmp(a, "--game-dir") && v) { dir = v; i++; }
        else if (!strcmp(a, "--scale") && v) { scale = atoi(v); i++; }
        else if (!strcmp(a, "--res-scale") && v) { res_scale = atoi(v); i++; }
        else if (!strcmp(a, "--fullscreen")) fullscreen = true;
        else if (!strcmp(a, "--check")) check = true;
        else if (!strcmp(a, "--bios-keys")) host_set_held_keys(false);
        else if (!strcmp(a, "--frame-rate") && v) { host_set_frame_rate(atoi(v)); rate_set = true; i++; }
        else if (!strcmp(a, "--sprite-detail") && v && !strcmp(v, "max")) { enh_sprite_detail_max = true; i++; }
        else if (!strcmp(a, "--sprite-detail") && v && !strcmp(v, "auto")) { enh_sprite_detail_max = false; i++; }
        else if (!strcmp(a, "--keys") && v) {
            if (!kb_parse(v)) { fprintf(stderr, "bad --keys list: %s\n", v); return usage(argv[0]); }
            i++;
        }
        else if (!strcmp(a, "--amiga") && v) {
            if (!amiga_parse(v)) { fprintf(stderr, "bad --amiga list: %s\n", v); return usage(argv[0]); }
            i++;
        }
        else if (!strcmp(a, "--amiga-disk") && v) { amiga_disk = v; i++; }
        else return usage(argv[0]);
    }

    char exe_path[1024];
    snprintf(exe_path, sizeof exe_path, "%s/TDEGA.EXE", dir);
    char err[256];
    if (!mem_load_exe(exe_path, err, sizeof err)) {
        fprintf(stderr, "%s\n", err);
        /* --check promises no window, and a modal box would wait for a click
           that a script cannot give. stderr already carries the reason. */
        if (!check) SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Test Drive", err, NULL);
        return 1;
    }
    if (check) {
        printf("TDEGA.EXE ok: image %u bytes at %04X:0000, DGROUP %04X\n", mem_image_size, LOAD_SEG, DGROUP);
        return 0;
    }

    if (!host_init(dir, scale)) return 1;
    if (fullscreen) host_set_fullscreen(true);
    gfx_set_output_scale(res_scale);
    enh_init();
    if (!rate_set) host_set_frame_rate(60);
    int rc = game_main();
    host_shutdown();
    return rc;
}
