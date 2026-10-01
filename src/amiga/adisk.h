#pragma once
/* The game disk: the files of the original Amiga disk, read either from an .adf disk image (AmigaDOS OFS/FFS,
 * as tools/adf.py reads it) or from a folder holding the extracted files. Paths are AmigaDOS paths relative to
 * the disk ("td", "cars/P911t.B", "df0:Pics/Title2"), looked up case-insensitively like AmigaDOS does. */
#include "../types.h"

/* `path` is an .adf file, or a folder with the extracted files (it has `td`), or a folder with an .adf in it.
 * On failure returns false and says why in err. */
bool adisk_open(const char *path, char *err, size_t errlen);
void adisk_close(void);

/* What was opened, for messages: the .adf file or the folder. */
const char *adisk_source(void);

/* Reads a whole file; returns a malloc'd buffer (free it) and its size, or NULL if it does not exist.
 * A file saved earlier by adisk_write is preferred over the one on the disk. */
u8 *adisk_read(const char *name, u32 *size);

/* Writes a file the game saves (HighScores). A disk image is never modified: the file goes to a folder beside
 * it, named after the image; in folder mode it is written into the game folder itself. False on failure. */
bool adisk_write(const char *name, const void *data, u32 size);

/* Calls fn for every file of the disk (paths with '/'). */
void adisk_list(void (*fn)(const char *path, void *ctx), void *ctx);
