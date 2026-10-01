/* The Amiga art as true-colour images for the DOS game's renderers (abridge.h: amiga_image, amiga_picture).
 * Shapes come from the disk's shape archives (FORMATS.md, Amiga section: the resource-archive layout big-endian;
 * a shape is u16 width_bytes, height, s16 hot_x, hot_y, x, y, u16 planes, plane_bytes, then the planes, MSB
 * leftmost), pictures are IFF ILBM (ByteRun1). Files are loaded through the engine's own loader, which unpacks
 * Pckd files. Images are kept until amiga_close. */
#include "abridge.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "amem.h"
#include "asymbols.h"
#include "platform/platform.h"

/* The road palette D:19CE (drive_scene.md §4.12): 12-bit 0RGB. */
const u16 AMIGA_ROAD_PALETTE[32] = {
    0x000, 0x00F, 0x673, 0x853, 0x3BF, 0xEE2, 0x743, 0x48F, 0xD32, 0x1D6, 0x686, 0x00F, 0xAAA, 0x999, 0x00B, 0x009,
    0xE00, 0x000, 0x111, 0x222, 0x333, 0x444, 0x555, 0x666, 0x777, 0x888, 0x999, 0xCCC, 0xFFF, 0x821, 0xB32, 0xF66,
};

u32 amiga_rgb(u16 c12)
{
    u32 r = (c12 >> 8) & 15, g = (c12 >> 4) & 15, b = c12 & 15;
    return (r * 17) << 16 | (g * 17) << 8 | (b * 17);
}

/* A file of the disk, unpacked, in a host buffer (free it); NULL if it is missing. */
static u8 *read_file(const char *name, u32 *size)
{
    APTR p = load_file(name);
    if (!p) return NULL;
    u32 n = D32(D_g_loadedSize);
    u8 *buf = malloc(n ? n : 1);
    if (buf)
        for (u32 i = 0; i < n; i++) buf[i] = rd8(p + i);
    free_mem(p);
    *size = n;
    return buf;
}

u8 *amiga_file(const char *name, u32 *size)
{
    *size = 0;
    return amiga_ready() ? read_file(name, size) : NULL;
}

static u16 be16(const u8 *p) { return (u16)(p[0] << 8 | p[1]); }
static u32 be32(const u8 *p) { return (u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | p[3]; }

typedef struct { char file[48]; u8 *data; u32 size; } Archive;
typedef struct { char file[48]; char name[5]; const u16 *pal; AmigaImage img; } Cached;

static Archive archives[16];
static int narchives;
static Cached images[256];
static int nimages;

static const Archive *archive(const char *file)
{
    for (int i = 0; i < narchives; i++)
        if (!strcmp(archives[i].file, file)) return &archives[i];
    if (narchives == (int)(sizeof archives / sizeof archives[0])) return NULL;
    Archive *a = &archives[narchives];
    a->data = read_file(file, &a->size);
    if (!a->data) return NULL;
    snprintf(a->file, sizeof a->file, "%s", file);
    narchives++;
    return a;
}

/* The shape `name` (4 characters, space padded) of an archive, or NULL. */
static const u8 *find_shape(const Archive *a, const char *name, u32 *avail)
{
    if (a->size < 6) return NULL;
    u16 n = be16(a->data + 4);
    u32 table = 6, data = 6 + 8u * n;
    if (data > a->size) return NULL;
    char key[4] = { ' ', ' ', ' ', ' ' };
    for (int i = 0; i < 4 && name[i]; i++) key[i] = name[i];
    for (u16 i = 0; i < n; i++) {
        const u8 *e = a->data + table + 4u * i;
        bool same = true;
        for (int k = 0; k < 4; k++) same &= e[k] == (u8)key[k] || (e[k] == 0 && key[k] == ' ');
        if (!same) continue;
        u32 off = data + be32(a->data + table + 4u * n + 4u * i);
        if (off + 16 > a->size) return NULL;
        *avail = a->size - off;
        return a->data + off;
    }
    return NULL;
}

const AmigaImage *amiga_image(const char *file, const char *name, const u16 *palette)
{
    if (!amiga_ready()) return NULL;
    if (!palette) palette = AMIGA_ROAD_PALETTE;
    for (int i = 0; i < nimages; i++)
        if (images[i].pal == palette && !strcmp(images[i].file, file) && !strncmp(images[i].name, name, 4))
            return images[i].img.px ? &images[i].img : NULL;
    if (nimages == (int)(sizeof images / sizeof images[0])) return NULL;
    Cached *c = &images[nimages++];
    memset(c, 0, sizeof *c);
    snprintf(c->file, sizeof c->file, "%s", file);
    snprintf(c->name, sizeof c->name, "%.4s", name);
    c->pal = palette;
    const Archive *a = archive(file);
    u32 avail = 0;
    const u8 *s = a ? find_shape(a, name, &avail) : NULL;
    if (!s) return NULL;
    int wb = be16(s), h = be16(s + 2), planes = be16(s + 12);
    u32 pb = be16(s + 14);
    if (wb <= 0 || h <= 0 || planes < 1 || planes > 5 || 16 + pb * planes > avail) return NULL;
    AmigaImage *im = &c->img;
    im->w = wb * 8;
    im->h = h;
    im->hx = (s16)be16(s + 4);
    im->hy = (s16)be16(s + 6);
    im->x = (s16)be16(s + 8);
    im->y = (s16)be16(s + 10);
    im->px = calloc((size_t)im->w * h, sizeof *im->px);
    im->idx = calloc((size_t)im->w * h, 1);
    if (!im->px || !im->idx) { free(im->px); free(im->idx); im->px = NULL; im->idx = NULL; return NULL; }
    for (int y = 0; y < h; y++)
        for (int x = 0; x < im->w; x++) {
            int v = 0;
            for (int p = 0; p < planes; p++)
                if (s[16 + pb * p + (u32)y * wb + (x >> 3)] & (0x80 >> (x & 7))) v |= 1 << p;
            im->px[y * im->w + x] = v ? 0xFF000000u | amiga_rgb(palette[v]) : 0;
            im->idx[y * im->w + x] = (u8)v;
        }
    return im;
}

/* ---- IFF ILBM pictures */

typedef struct { char file[48]; AmigaPicture pic; bool tried; } CachedPic;
static CachedPic pictures[16];
static int npictures;

const AmigaPicture *amiga_picture(const char *file)
{
    if (!amiga_ready()) return NULL;
    for (int i = 0; i < npictures; i++)
        if (!strcmp(pictures[i].file, file)) return pictures[i].pic.px ? &pictures[i].pic : NULL;
    if (npictures == (int)(sizeof pictures / sizeof pictures[0])) return NULL;
    CachedPic *c = &pictures[npictures++];
    memset(c, 0, sizeof *c);
    snprintf(c->file, sizeof c->file, "%s", file);
    u32 size = 0;
    u8 *d = read_file(file, &size);
    if (!d) return NULL;
    if (size < 12 || memcmp(d, "FORM", 4) || memcmp(d + 8, "ILBM", 4)) { free(d); return NULL; }
    int w = 0, h = 0, planes = 0, comp = 0, ncol = 0;
    const u8 *body = NULL;
    u32 body_len = 0;
    for (u32 p = 12; p + 8 <= size; ) {
        u32 len = be32(d + p + 4);
        const u8 *ck = d + p + 8;
        if (p + 8 + len > size) break;
        if (!memcmp(d + p, "BMHD", 4) && len >= 20) {
            w = be16(ck); h = be16(ck + 2); planes = ck[8]; comp = ck[10];
        } else if (!memcmp(d + p, "CMAP", 4)) {
            ncol = (int)(len / 3) > 32 ? 32 : (int)(len / 3);
            for (int i = 0; i < ncol; i++)
                c->pic.palette[i] = (u16)((ck[3 * i] >> 4) << 8 | (ck[3 * i + 1] >> 4) << 4 | (ck[3 * i + 2] >> 4));
        } else if (!memcmp(d + p, "BODY", 4)) {
            body = ck;
            body_len = len;
        }
        p += 8 + ((len + 1) & ~1u);
    }
    if (!body || w <= 0 || h <= 0 || planes < 1 || planes > 5) { free(d); return NULL; }
    int rb = ((w + 15) / 16) * 2;
    u8 *rows = calloc((size_t)rb * planes * h, 1);
    if (!rows) { free(d); return NULL; }
    /* ByteRun1 (comp 1) or none, row by row, plane by plane */
    u32 in = 0, out = 0, total = (u32)rb * planes * h;
    while (out < total && in < body_len) {
        if (comp != 1) { rows[out++] = body[in++]; continue; }
        s8 n = (s8)body[in++];
        if (n >= 0) {
            for (int k = 0; k <= n && in < body_len && out < total; k++) rows[out++] = body[in++];
        } else if (n != -128 && in < body_len) {
            u8 b = body[in++];
            for (int k = 0; k <= -n && out < total; k++) rows[out++] = b;
        }
    }
    AmigaPicture *pic = &c->pic;
    pic->w = w;
    pic->h = h;
    pic->index = calloc((size_t)w * h, 1);
    pic->px = calloc((size_t)w * h, sizeof *pic->px);
    if (!pic->index || !pic->px) { free(rows); free(d); free(pic->index); free(pic->px); pic->px = NULL; return NULL; }
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            int v = 0;
            for (int p = 0; p < planes; p++)
                if (rows[((size_t)y * planes + p) * rb + (x >> 3)] & (0x80 >> (x & 7))) v |= 1 << p;
            pic->index[y * w + x] = (u8)v;
            pic->px[y * w + x] = amiga_rgb(pic->palette[v]);
        }
    free(rows);
    free(d);
    return pic;
}

void amiga_images_free(void)
{
    for (int i = 0; i < nimages; i++) { free(images[i].img.px); free(images[i].img.idx); }
    for (int i = 0; i < narchives; i++) free(archives[i].data);
    for (int i = 0; i < npictures; i++) { free(pictures[i].pic.px); free(pictures[i].pic.index); }
    nimages = narchives = npictures = 0;
}
