/* Amiga memory image: the hunk loader (tools/hunk.py in C) and exec AllocMem/FreeMem. */
#include "amem.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ahost.h"

u8 amem[AMEM_SIZE];
u32 amem_image_end;

_Noreturn void amem_fault(u32 addr)
{
    ahost_fatal("Internal error: memory access at %08X, outside the emulated 68000 memory.", addr);
}

void amem_put(APTR dst, const void *src, u32 n) { if (n) { ap(dst + n - 1); memcpy(ap(dst), src, n); } }
void amem_get(void *dst, APTR src, u32 n)       { if (n) { ap(src + n - 1); memcpy(dst, ap(src), n); } }
void amem_set(APTR dst, u8 v, u32 n)            { if (n) { ap(dst + n - 1); memset(ap(dst), v, n); } }

/* ---------------------------------------------------------------- hunk loader */

#define HUNK_NAME     0x3E8
#define HUNK_CODE     0x3E9
#define HUNK_DATA     0x3EA
#define HUNK_BSS      0x3EB
#define HUNK_RELOC32  0x3EC
#define HUNK_SYMBOL   0x3F0
#define HUNK_DEBUG    0x3F1
#define HUNK_END      0x3F2
#define HUNK_HEADER   0x3F3
#define HUNK_OVERLAY  0x3F5
#define HUNK_BREAK    0x3F6
#define HUNK_DREL32   0x3F7
#define HUNK_RELOC32SHORT 0x3FC

#define MAX_HUNKS 16

typedef struct {
    u32 size;                       /* allocation size in bytes, from the header */
    const u8 *data;                 /* contents in the file buffer (CODE/DATA) */
    u32 data_len;
    const char *kind;
    bool overlay;
    u32 base;
} Hunk;

static Hunk hunks[MAX_HUNKS];
static int nhunks;

typedef struct { const u8 *b; size_t len, pos; bool bad; } Reader;

static u32 get32(Reader *r)
{
    if (r->pos + 4 > r->len) { r->bad = true; r->pos = r->len; return 0; }
    const u8 *p = r->b + r->pos;
    r->pos += 4;
    return (u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | p[3];
}

static u16 get16(Reader *r)
{
    if (r->pos + 2 > r->len) { r->bad = true; r->pos = r->len; return 0; }
    const u8 *p = r->b + r->pos;
    r->pos += 2;
    return (u16)(p[0] << 8 | p[1]);
}

/* A relocation, applied after all hunks are placed. */
typedef struct { int hunk, target; u32 offset; } Reloc;

static bool parse(Reader *r, Reloc **relocs, int *nrel, char *err, size_t errlen)
{
    int cap = 0;
    *relocs = NULL;
    *nrel = 0;
    if (get32(r) != HUNK_HEADER) { snprintf(err, errlen, "not an AmigaDOS load file"); return false; }
    while (get32(r) && !r->bad) {}                             /* resident library names */
    get32(r);                                                  /* table size */
    u32 first = get32(r), last = get32(r);
    if (r->bad || last < first || last >= MAX_HUNKS) { snprintf(err, errlen, "bad hunk header"); return false; }
    for (u32 i = first; i <= last; i++) hunks[i].size = (get32(r) & 0x3FFFFFFF) * 4;
    nhunks = (int)last + 1;
    int cur = (int)first;
    while (r->pos < r->len && !r->bad) {
        u32 t = get32(r) & 0x3FFFFFFF;
        Hunk *h = cur < MAX_HUNKS ? &hunks[cur] : NULL;
        switch (t) {
        case HUNK_CODE:
        case HUNK_DATA: {
            u32 n = get32(r) * 4;
            if (!h || r->pos + n > r->len) { r->bad = true; break; }
            h->kind = t == HUNK_CODE ? "CODE" : "DATA";
            h->data = r->b + r->pos;
            h->data_len = n;
            r->pos += n;
            break;
        }
        case HUNK_BSS:
            get32(r);
            if (h) h->kind = "BSS";
            break;
        case HUNK_RELOC32:
        case HUNK_DREL32:
            for (;;) {
                u32 n = get32(r);
                if (!n || r->bad) break;
                u32 target = get32(r);
                for (u32 k = 0; k < n && !r->bad; k++) {
                    if (*nrel == cap) { cap = cap ? cap * 2 : 1024; *relocs = realloc(*relocs, cap * sizeof **relocs); }
                    (*relocs)[(*nrel)++] = (Reloc){ cur, (int)target, get32(r) };
                }
            }
            break;
        case HUNK_RELOC32SHORT:
            for (;;) {
                u16 n = get16(r);
                if (!n || r->bad) break;
                u16 target = get16(r);
                for (u16 k = 0; k < n && !r->bad; k++) {
                    if (*nrel == cap) { cap = cap ? cap * 2 : 1024; *relocs = realloc(*relocs, cap * sizeof **relocs); }
                    (*relocs)[(*nrel)++] = (Reloc){ cur, target, get16(r) };
                }
            }
            r->pos = (r->pos + 3) & ~(size_t)3;
            break;
        case HUNK_SYMBOL:
            for (;;) {
                u32 n = get32(r);
                if (!n || r->bad) break;
                r->pos += n * 4;
                get32(r);
            }
            break;
        case HUNK_DEBUG:
        case HUNK_NAME:
            r->pos += get32(r) * 4;
            break;
        case HUNK_END:
            cur++;
            break;
        case HUNK_OVERLAY: {                                   /* overlay table: not needed, everything is resident */
            u32 n = get32(r);
            r->pos += (n + 1) * 4;
            break;
        }
        case HUNK_HEADER: {                                    /* an overlay node: more hunks */
            while (get32(r) && !r->bad) {}
            get32(r);
            u32 f = get32(r), l = get32(r);
            if (r->bad || l < f || l >= MAX_HUNKS) { r->bad = true; break; }
            for (u32 i = f; i <= l; i++) {
                hunks[i].size = (get32(r) & 0x3FFFFFFF) * 4;
                hunks[i].overlay = true;
            }
            if ((int)l + 1 > nhunks) nhunks = (int)l + 1;
            cur = (int)f;
            break;
        }
        case HUNK_BREAK:
            break;
        default:
            snprintf(err, errlen, "unhandled hunk type %X", t);
            return false;
        }
    }
    if (r->bad) { snprintf(err, errlen, "the load file is truncated or damaged"); return false; }
    return true;
}

bool amem_load_exe(const u8 *buf, size_t len, char *err, size_t errlen)
{
    memset(hunks, 0, sizeof hunks);
    nhunks = 0;
    Reader r = { buf, len, 0, false };
    Reloc *relocs = NULL;
    int nrel = 0;
    char why[128] = "";
    if (!parse(&r, &relocs, &nrel, why, sizeof why)) {
        snprintf(err, errlen, "td: %s.", why);
        free(relocs);
        return false;
    }

    memset(amem, 0, sizeof amem);
    u32 addr = IMAGE_BASE;
    for (int i = 0; i < nhunks; i++) {
        hunks[i].base = addr;
        addr = (addr + hunks[i].size + 15) & ~15u;
    }
    amem_image_end = addr;
    for (int i = 0; i < nhunks; i++)
        if (hunks[i].data) memcpy(amem + hunks[i].base, hunks[i].data,
                                  hunks[i].data_len < hunks[i].size ? hunks[i].data_len : hunks[i].size);
    for (int k = 0; k < nrel; k++) {
        const Reloc *rl = &relocs[k];
        if (rl->target >= nhunks || rl->offset + 4 > hunks[rl->hunk].size) continue;
        APTR at = hunks[rl->hunk].base + rl->offset;
        wr32(at, rd32(at) + hunks[rl->target].base);
    }
    free(relocs);
    for (int i = 0; i < nhunks; i++) hunks[i].data = NULL;     /* pointed into buf */

    /* The specs' addresses assume the layout of work/amiga/td.bin. */
    if (nhunks != 5 || hunks[1].base != DATA_HUNK || hunks[3].base != 0x1AED0 || hunks[4].base != 0x1C900) {
        snprintf(err, errlen, "td is not the Amiga Test Drive executable this port was made from.");
        return false;
    }
    return true;
}

int amem_hunk_count(void) { return nhunks; }

void amem_hunk_info(int i, u32 *base, u32 *size, const char **kind, bool *overlay)
{
    *base = hunks[i].base;
    *size = hunks[i].size;
    *kind = hunks[i].kind ? hunks[i].kind : "?";
    *overlay = hunks[i].overlay;
}

/* ---------------------------------------------------------------- AllocMem / FreeMem */

/* Free list of the heap [amem_image_end rounded up, AMEM_SIZE), address-ordered, blocks of 8-byte units. */
#define MAX_FREE 4096
static struct { u32 addr, size; } free_list[MAX_FREE];
static int nfree = -1;

static void heap_init(void)
{
    u32 start = (amem_image_end + 0xFFF) & ~0xFFFu;
    free_list[0].addr = start;
    free_list[0].size = AMEM_SIZE - start;
    nfree = 1;
}

APTR exec_AllocMem(u32 size, u32 flags)
{
    if (nfree < 0) heap_init();
    if (size == 0) return 0;
    size = (size + 7) & ~7u;
    for (int i = 0; i < nfree; i++) {
        if (free_list[i].size < size) continue;
        APTR a = free_list[i].addr;
        free_list[i].addr += size;
        free_list[i].size -= size;
        if (free_list[i].size == 0) {
            memmove(&free_list[i], &free_list[i + 1], (size_t)(nfree - i - 1) * sizeof free_list[0]);
            nfree--;
        }
        if (flags & MEMF_CLEAR) memset(amem + a, 0, size);
        return a;
    }
    return 0;
}

void exec_FreeMem(APTR a, u32 size)
{
    if (!a || !size) return;
    if (nfree < 0) heap_init();
    size = (size + 7) & ~7u;
    int i = 0;
    while (i < nfree && free_list[i].addr < a) i++;
    bool join_prev = i > 0 && free_list[i - 1].addr + free_list[i - 1].size == a;
    bool join_next = i < nfree && a + size == free_list[i].addr;
    if (join_prev && join_next) {
        free_list[i - 1].size += size + free_list[i].size;
        memmove(&free_list[i], &free_list[i + 1], (size_t)(nfree - i - 1) * sizeof free_list[0]);
        nfree--;
    } else if (join_prev) {
        free_list[i - 1].size += size;
    } else if (join_next) {
        free_list[i].addr = a;
        free_list[i].size += size;
    } else {
        if (nfree == MAX_FREE) return;                         /* leak rather than corrupt */
        memmove(&free_list[i + 1], &free_list[i], (size_t)(nfree - i) * sizeof free_list[0]);
        free_list[i].addr = a;
        free_list[i].size = size;
        nfree++;
    }
}

u32 exec_AvailMem(void)
{
    if (nfree < 0) heap_init();
    u32 best = 0;
    for (int i = 0; i < nfree; i++) if (free_list[i].size > best) best = free_list[i].size;
    return best;
}
