#pragma once
/* 68000 memory model of the Amiga port.
 *
 * The game's load file `td` is laid out in amem[] exactly as tools/hunk.py lays out work/amiga/td.bin: hunks from
 * IMAGE_BASE (0x10000), each aligned to 16 bytes, all overlays resident, RELOC32 applied. So every address in the
 * specs is an address here: code 0x10000-0x17C0C, the data hunk from DATA_HUNK (0x17C10: globals D:xxxx are at
 * DATA_HUNK + xxxx), overlay 1 at 0x1AED0, overlay 2 at 0x1C900. Everything the game allocates (bitmaps, loaded
 * files, samples, ColorMaps, copper lists) comes from the AllocMem heap above the image. Memory is big-endian, as
 * on the 68000: always go through the accessors. There is no chip/fast distinction (the custom chips are
 * emulated host-side and read amem[] directly).
 */
#include "../types.h"

#define AMEM_SIZE   0x200000u       /* 2 MB: the image, then the heap */
#define IMAGE_BASE  0x10000u
#define DATA_HUNK   0x17C10u        /* D:0000 */
#define A4_VALUE    0x1FC0Eu        /* A4 = DATA_HUNK + 0x7FFE: d16(A4) is D:(0x7FFE + d16) */

typedef u32 APTR;                   /* a 68000 address in amem[]; 0 is NULL */

extern u8 amem[AMEM_SIZE];
extern u32 amem_image_end;          /* first address after the loaded image */

_Noreturn void amem_fault(u32 addr);

static inline u8 *ap(APTR a) { if (a >= AMEM_SIZE) amem_fault(a); return amem + a; }

static inline u8  rd8 (APTR a) { return *ap(a); }
static inline u16 rd16(APTR a) { u8 *p = ap(a); return (u16)(p[0] << 8 | p[1]); }
static inline u32 rd32(APTR a) { u8 *p = ap(a); return (u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | p[3]; }
static inline void wr8 (APTR a, u8 v)  { *ap(a) = v; }
static inline void wr16(APTR a, u16 v) { u8 *p = ap(a); p[0] = (u8)(v >> 8); p[1] = (u8)v; }
static inline void wr32(APTR a, u32 v) { u8 *p = ap(a); p[0] = (u8)(v >> 24); p[1] = (u8)(v >> 16); p[2] = (u8)(v >> 8); p[3] = (u8)v; }

/* Globals D:xxxx (offsets from asymbols.h, e.g. D16(D_tick_count)). */
#define DADDR(o)      (DATA_HUNK + (u32)(o))
#define D8(o)         rd8(DADDR(o))
#define D16(o)        rd16(DADDR(o))
#define DS16(o)       ((s16)rd16(DADDR(o)))
#define D32(o)        rd32(DADDR(o))
#define DS32(o)       ((s32)rd32(DADDR(o)))
#define SETD8(o, v)   wr8(DADDR(o), (u8)(v))
#define SETD16(o, v)  wr16(DADDR(o), (u16)(v))
#define SETD32(o, v)  wr32(DADDR(o), (u32)(v))

/* Loads the AmigaDOS load file (hunk format, with overlays) into amem[]. On failure returns false and says
 * why in err. */
bool amem_load_exe(const u8 *file, size_t len, char *err, size_t errlen);

/* Hunk addresses of the loaded image (for --check). */
int  amem_hunk_count(void);
void amem_hunk_info(int i, u32 *base, u32 *size, const char **kind, bool *overlay);

/* ---- exec AllocMem / FreeMem on the heap above the image. The flags are accepted but only MEMF_CLEAR matters. */
#define MEMF_PUBLIC  0x0001u
#define MEMF_CHIP    0x0002u
#define MEMF_FAST    0x0004u
#define MEMF_CLEAR   0x10000u
APTR exec_AllocMem(u32 size, u32 flags);        /* 0 when out of memory */
void exec_FreeMem(APTR addr, u32 size);
u32  exec_AvailMem(void);                       /* largest free block */

/* Copies between host memory and amem[]. */
void amem_put(APTR dst, const void *src, u32 n);
void amem_get(void *dst, APTR src, u32 n);
void amem_set(APTR dst, u8 v, u32 n);
