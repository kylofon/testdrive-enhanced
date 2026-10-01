/* The game disk: see adisk.h. */
#include "adisk.h"

#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BSIZE     512
#define ROOT      880
#define ST_DIR    2
#define ST_FILE   (-3)

static u8 *adf;                 /* the disk image, or NULL in folder mode */
static size_t adf_blocks;
static bool adf_ffs;
static char *folder;            /* folder mode: the folder with the extracted files */
static char *saves;             /* .adf mode: the folder beside the image where saved files go */
static char *source;

const char *adisk_source(void) { return source ? source : ""; }

/* ---------------------------------------------------------------- .adf */

static s32 blong(u32 block, int i)
{
    if (block >= adf_blocks) return 0;
    const u8 *p = adf + (size_t)block * BSIZE + 4 * i;
    return (s32)((u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | p[3]);
}

static void bname(u32 block, char *out, size_t n)
{
    const u8 *b = adf + (size_t)block * BSIZE;
    size_t len = b[432] < 30 ? b[432] : 30;
    if (len >= n) len = n - 1;
    memcpy(out, b + 433, len);
    out[len] = '\0';
}

/* The entry `name` of the directory at `dir` (case-insensitive), or 0. */
static u32 adf_find(u32 dir, const char *name)
{
    for (int i = 6; i < 78; i++) {
        int guard = 0;
        for (u32 h = (u32)blong(dir, i); h && h < adf_blocks && guard++ < 1000; h = (u32)blong(h, 124)) {
            char n[32];
            bname(h, n, sizeof n);
            if (SDL_strcasecmp(n, name) == 0) return h;
        }
    }
    return 0;
}

static u8 *adf_file(u32 hdr, u32 *size)
{
    u32 len = (u32)blong(hdr, 81);
    if (len > adf_blocks * BSIZE) return NULL;
    u8 *out = malloc(len ? len : 1);
    u32 got = 0;
    for (u32 h = hdr; h && h < adf_blocks && got < len; h = (u32)blong(h, 126)) {
        s32 count = blong(h, 2);
        for (s32 i = 0; i < count && i < 72 && got < len; i++) {
            u32 b = (u32)blong(h, 77 - i);
            if (b >= adf_blocks) break;
            const u8 *d = adf + (size_t)b * BSIZE;
            u32 n = adf_ffs ? BSIZE : (u32)blong(b, 3);
            if (n > (adf_ffs ? BSIZE : BSIZE - 24)) n = 0;
            if (n > len - got) n = len - got;
            memcpy(out + got, d + (adf_ffs ? 0 : 24), n);
            got += n;
        }
    }
    if (got < len) { free(out); return NULL; }
    *size = len;
    return out;
}

static void adf_walk(u32 dir, const char *prefix, void (*fn)(const char *, void *), void *ctx, int depth)
{
    if (depth > 8) return;
    for (int i = 6; i < 78; i++) {
        int guard = 0;
        for (u32 h = (u32)blong(dir, i); h && h < adf_blocks && guard++ < 1000; h = (u32)blong(h, 124)) {
            char n[32], path[512];
            bname(h, n, sizeof n);
            SDL_snprintf(path, sizeof path, "%s%s%s", prefix, *prefix ? "/" : "", n);
            if (blong(h, 127) == ST_DIR) adf_walk(h, path, fn, ctx, depth + 1);
            else if (blong(h, 127) == ST_FILE) fn(path, ctx);
        }
    }
}

/* ---------------------------------------------------------------- folder */

static char *find_in_dir(const char *dir, const char *name)
{
    char *direct = NULL;
    SDL_asprintf(&direct, "%s/%s", dir, name);
    if (SDL_GetPathInfo(direct, NULL)) return direct;
    SDL_free(direct);
    int count = 0;
    char **entries = SDL_GlobDirectory(dir, NULL, 0, &count);
    char *found = NULL;
    for (int i = 0; entries && i < count; i++)
        if (SDL_strcasecmp(entries[i], name) == 0) {
            SDL_asprintf(&found, "%s/%s", dir, entries[i]);
            break;
        }
    SDL_free(entries);
    return found;
}

/* Splits an AmigaDOS path into components ("df0:" and ":" mean the disk root). */
static int split(const char *name, char parts[][32], int max)
{
    const char *colon = SDL_strchr(name, ':');
    if (colon) name = colon + 1;
    int n = 0;
    while (*name && n < max) {
        while (*name == '/') name++;
        if (!*name) break;
        size_t len = strcspn(name, "/");
        size_t k = len < 31 ? len : 31;
        memcpy(parts[n], name, k);
        parts[n][k] = '\0';
        n++;
        name += len;
    }
    return n;
}

/* ---------------------------------------------------------------- open, read */

static bool load_adf(const char *path, char *err, size_t errlen)
{
    size_t len = 0;
    u8 *data = SDL_LoadFile(path, &len);
    if (!data) { snprintf(err, errlen, "Cannot read the disk image %s.", path); return false; }
    if (len < (ROOT + 1) * BSIZE || memcmp(data, "DOS", 3) != 0) {
        SDL_free(data);
        snprintf(err, errlen, "%s is not an AmigaDOS disk image.", path);
        return false;
    }
    adf = data;
    adf_blocks = len / BSIZE;
    adf_ffs = (data[3] & 1) != 0;
    return true;
}

/* "<image>.adf" -> the folder "<image> files" beside it, for what the game saves. */
static void set_saves_folder(const char *adf_path)
{
    char *stem = SDL_strdup(adf_path);
    char *dot = SDL_strrchr(stem, '.');
    const char sep = 92;                 /* the other path separator on Windows */
    if (dot && !SDL_strchr(dot, '/') && !SDL_strchr(dot, sep)) *dot = 0;
    SDL_free(saves);
    SDL_asprintf(&saves, "%s files", stem);
    SDL_free(stem);
}

bool adisk_open(const char *path, char *err, size_t errlen)
{
    adisk_close();
    SDL_PathInfo info;
    if (!SDL_GetPathInfo(path, &info)) {
        snprintf(err, errlen, "The game folder %s does not exist.", path);
        return false;
    }
    if (info.type == SDL_PATHTYPE_FILE) {
        if (!load_adf(path, err, errlen)) return false;
        source = SDL_strdup(path);
        set_saves_folder(path);
        return true;
    }
    char *td = find_in_dir(path, "td");
    if (td) {
        SDL_free(td);
        folder = SDL_strdup(path);
        source = SDL_strdup(path);
        return true;
    }
    int count = 0;
    char **entries = SDL_GlobDirectory(path, "*.adf", SDL_GLOB_CASEINSENSITIVE, &count);
    bool ok = false;
    for (int i = 0; entries && i < count && !ok; i++) {
        char *file = NULL;
        SDL_asprintf(&file, "%s/%s", path, entries[i]);
        char why[256];
        if (load_adf(file, why, sizeof why)) {
            source = file;
            set_saves_folder(file);
            ok = true;
        } else {
            SDL_free(file);
        }
    }
    SDL_free(entries);
    if (!ok)
        snprintf(err, errlen, "%s has neither the Amiga disk image (.adf) nor the files from the disk (td, Cars, "
                 "Pics, ...).", path);
    return ok;
}

void adisk_close(void)
{
    SDL_free(adf);
    SDL_free(folder);
    SDL_free(saves);
    SDL_free(source);
    adf = NULL;
    folder = NULL;
    saves = NULL;
    source = NULL;
}

/* The host path a saved file would have, or NULL when there is nowhere to save. */
static char *save_path(const char *name)
{
    const char *base = SDL_strrchr(name, '/');
    base = base ? base + 1 : name;
    char *path = NULL;
    if (folder) {
        path = find_in_dir(folder, base);
        if (!path) SDL_asprintf(&path, "%s/%s", folder, base);
    } else if (saves) {
        SDL_asprintf(&path, "%s/%s", saves, base);
    }
    return path;
}

bool adisk_write(const char *name, const void *data, u32 size)
{
    if (saves) SDL_CreateDirectory(saves);
    char *path = save_path(name);
    if (!path) return false;
    bool ok = SDL_SaveFile(path, data, size);
    SDL_free(path);
    return ok;
}

u8 *adisk_read(const char *name, u32 *size)
{
    char parts[8][32];
    int n = split(name, parts, 8);
    if (n == 0) return NULL;
    if (adf) {                                                 /* a file saved earlier wins over the image */
        char *sp = save_path(name);
        size_t len = 0;
        u8 *data = sp ? SDL_LoadFile(sp, &len) : NULL;
        SDL_free(sp);
        if (data) {
            u8 *out = malloc(len ? len : 1);
            memcpy(out, data, len);
            SDL_free(data);
            *size = (u32)len;
            return out;
        }
    }
    if (adf) {
        u32 b = ROOT;
        for (int i = 0; i < n && b; i++) b = adf_find(b, parts[i]);
        if (!b || blong(b, 127) != ST_FILE) return NULL;
        return adf_file(b, size);
    }
    if (!folder) return NULL;
    char *path = SDL_strdup(folder);
    for (int i = 0; i < n && path; i++) {
        char *next = find_in_dir(path, parts[i]);
        SDL_free(path);
        path = next;
    }
    if (!path) return NULL;
    size_t len = 0;
    u8 *data = SDL_LoadFile(path, &len);
    SDL_free(path);
    if (!data) return NULL;
    u8 *out = malloc(len ? len : 1);
    memcpy(out, data, len);
    SDL_free(data);
    *size = (u32)len;
    return out;
}

void adisk_list(void (*fn)(const char *, void *), void *ctx)
{
    if (adf) {
        adf_walk(ROOT, "", fn, ctx, 0);
        return;
    }
    if (!folder) return;
    int count = 0;
    char **entries = SDL_GlobDirectory(folder, NULL, 0, &count);
    for (int i = 0; entries && i < count; i++) {
        char *full = NULL;
        SDL_asprintf(&full, "%s/%s", folder, entries[i]);
        SDL_PathInfo info;
        bool file = SDL_GetPathInfo(full, &info) && info.type == SDL_PATHTYPE_FILE;
        SDL_free(full);
        if (!file) continue;
        char p[512];
        SDL_strlcpy(p, entries[i], sizeof p);
        for (char *c = p; *c; c++) if (*c == '\\') *c = '/';
        fn(p, ctx);
    }
    SDL_free(entries);
}
