/* Test Drive Enhanced — entry point.
 *
 * usage: testdrive-enhanced [--game-dir DIR] [--scale N] [--res-scale N] [--frame-rate FPS]
 *                           [--sprite-detail max|auto] [--bios-keys] [--check]
 *   --res-scale  output resolution as a multiple of 320x200 (default 4, 1..8)
 *   --frame-rate drawing rate while driving (default 60; 0 = as fast as possible)
 *   --sprite-detail max (default): the most detailed sprite of every car and object at every distance;
 *                auto: the sprite scale chosen by distance
 *   --bios-keys driving keys act only through key repeat, exactly like the original (default: held keys)
 *   --game-dir  folder with the original game files (default: "Game" next to the working directory)
 *   --scale     initial window scale (default 3)
 *   --check     load and verify TDEGA.EXE, print a summary and exit (no window)
 */
#define SDL_MAIN_HANDLED
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "host.h"
#include "mem.h"
#include "enhanced/enhanced.h"
#include "platform/gfx.h"

int game_main(void);   /* game/flow.c: port of main() at image 0x0010 */

int main(int argc, char **argv)
{
    const char *dir = "Game";
    int scale = 3, res_scale = 4;
    bool check = false, rate_set = false;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--game-dir") && i + 1 < argc) dir = argv[++i];
        else if (!strcmp(argv[i], "--scale") && i + 1 < argc) scale = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--res-scale") && i + 1 < argc) res_scale = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--check")) check = true;
        else if (!strcmp(argv[i], "--bios-keys")) host_set_held_keys(false);
        else if (!strcmp(argv[i], "--frame-rate") && i + 1 < argc) { host_set_frame_rate(atoi(argv[++i])); rate_set = true; }
        else if (!strcmp(argv[i], "--sprite-detail") && i + 1 < argc && !strcmp(argv[i + 1], "max")) { enh_sprite_detail_max = true; i++; }
        else if (!strcmp(argv[i], "--sprite-detail") && i + 1 < argc && !strcmp(argv[i + 1], "auto")) { enh_sprite_detail_max = false; i++; }
        else {
            fprintf(stderr, "usage: %s [--game-dir DIR] [--scale N] [--res-scale N] [--frame-rate FPS] "
                            "[--sprite-detail max|auto] [--bios-keys] [--check]\n", argv[0]);
            return 2;
        }
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
    gfx_set_output_scale(res_scale);
    enh_init();
    if (!rate_set) host_set_frame_rate(60);
    int rc = game_main();
    host_shutdown();
    return rc;
}
