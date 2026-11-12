#pragma once
/* The Amiga engine as the DOS game uses it (--amiga, ../amiga_opts.h): its disk and its program loaded beside the
 * DOS game, and the parts the Amiga features call. Everything else in src/amiga is the engine itself, with its
 * names prefixed (anames.h); these functions are not. */
#include "../types.h"

/* Opens the Amiga disk (amiga_disk), loads td into the engine's memory and starts the engine (display, Paula,
 * vertical blank, input, sounds, songs, Cars.txt) without showing anything. False with the reason in err. */
bool amiga_open(char *err, size_t errlen);
bool amiga_ready(void);
void amiga_close(void);

/* The engine's screens are shown and get the keys (the DOS game's picture and keyboard are set aside). */
void amiga_screens_begin(void);
void amiga_screens_end(void);

/* The title sequence (td 0x1AED0). Returns -1 when it ran to the end, 0 when a key or fire stopped it. */
int amiga_intro(void);
/* Car selection and showroom (td 0x1B8E6), in attract mode (a 9 s timeout instead of 100 s) when `demo`. Returns
 * the Amiga car index, -1 on the timeout or -2 when Ctrl-R was pressed. */
int amiga_car_select(bool demo);
/* "Play again / new car" (td 0x1D5C8). Returns 1 for the same car, 0 for a new one. */
int amiga_play_again(void);
void amiga_stage_results(s16 car, s16 stage, s32 avgSpeed, s16 secs, s32 score, s16 limit);
/* The car file of an Amiga car index ("cars/P911t"), empty if there is none; and the number of cars. */
const char *amiga_car_file(int idx);
int amiga_car_count(void);

/* ---- The Amiga art (aimage.c), for the DOS game's renderers. */
typedef struct {
    int w, h, hx, hy, x, y;        /* size, hot spot, and where the cockpit code draws it */
    u32 *px;                       /* ARGB; alpha 0 = colour 0 (transparent), else 0xFF */
    u8 *idx;                       /* the colour index of each pixel (for XOR drawing) */
} AmigaImage;
typedef struct {
    int w, h;
    u16 palette[32];               /* the CMAP, 12-bit 0RGB */
    u8 *index;                     /* colour index per pixel */
    u32 *px;                       /* XRGB */
} AmigaPicture;
extern const u16 AMIGA_ROAD_PALETTE[32];   /* D:19CE */
u32 amiga_rgb(u16 c12);                    /* 12-bit 0RGB to XRGB8888 */
/* The shape `name` (4 characters) of the shape archive `file` ("Pics/Road.Shp"), in `palette` (32 colours,
 * 12-bit; NULL = the road palette). NULL when the disk or the shape isn't there. Kept until amiga_close. */
const AmigaImage *amiga_image(const char *file, const char *name, const u16 *palette);
/* An IFF ILBM picture of the disk ("Cars/P911tDash"), or NULL. */
const AmigaPicture *amiga_picture(const char *file);
void amiga_images_free(void);
/* A file of the disk, unpacked, in a malloc'd buffer (free it), or NULL. */
u8 *amiga_file(const char *name, u32 *size);

/* The songs (Amiga music). */
enum { AMIGA_SONG_TESTDRIVE, AMIGA_SONG_TEST2, AMIGA_SONG_TESTGAS, AMIGA_SONG_ENDSUCCESS };
void amiga_song_play(int song);
void amiga_song_stop(void);
bool amiga_song_playing(void);
