/* Files and memory — port of td 0x149D2-0x151D6 (load_file, Pckd) and 0x15904-0x159FA (mem_alloc, free_mem)
 * (port/amiga/spec/platform_video.md §4.12, §4.13; FORMATS.md, Amiga section). */
#include "platform.h"

#include <stdlib.h>
#include <string.h>

#include "../adisk.h"
#include "../aport.h"
#include "../asymbols.h"

/* ---------------------------------------------------------------- memory */

#define MEMB 0x4D656D42u                                /* 'MemB' */
#define MEME 0x4D656D45u                                /* 'MemE' */

/* Block: +0 alloc size, +4 next, +8 prev (circular list from D:1BDC), +0xC 'MemB', data at +0x10, 'MemE' last. */
APTR mem_alloc(u32 size, u32 flags)
{
    size = (size + 0x17) & ~3u;
    APTR b = exec_AllocMem(size, flags | MEMF_CLEAR);
    if (!b) return 0;
    SETD16(D_mem_count, D16(D_mem_count) + 1);
    wr32(b, size);
    APTR head = D32(D_mem_list);
    if (!head) {
        SETD32(D_mem_list, b);
        wr32(b + 4, b);
        wr32(b + 8, b);
    } else {
        APTR prev = rd32(head + 8);
        wr32(b + 4, head);
        wr32(b + 8, prev);
        wr32(prev + 4, b);
        wr32(head + 8, b);
    }
    wr32(b + 0xC, MEMB);
    wr32(b + (size & ~3u) - 4, MEME);
    return b + 0x10;
}

APTR alloc_public(u32 size) { return mem_alloc(size, 0x10001); }
APTR alloc_chip(u32 size)   { return mem_alloc(size, 0x10003); }

s16 free_mem(APTR p)
{
    if (!p) return 0;
    if (p == 0xFFFFFFFFu) {
        while (D32(D_mem_list) && free_mem(D32(D_mem_list) + 0x10) == 0) {}
        return 0;
    }
    APTR b = p - 0x10;
    if ((b & 3) || rd32(b + 0xC) != MEMB || rd32(rd32(b + 4) + 0xC) != MEMB || rd32(rd32(b + 8) + 0xC) != MEMB)
        return -1;
    /* the 'MemE' comparison is made but its result is not used */
    if (b == D32(D_mem_list)) SETD32(D_mem_list, rd32(b + 4));
    if (b == rd32(b + 4)) SETD32(D_mem_list, 0);
    u32 size = rd32(b);
    wr32(b + 0xC, 0);
    wr32(b + (size & ~3u) - 4, 0);
    APTR next = rd32(b + 4), prev = rd32(b + 8);
    wr32(prev + 4, next);
    wr32(next + 8, prev);
    exec_FreeMem(b, size);
    SETD16(D_mem_count, D16(D_mem_count) - 1);
    return 0;
}

/* ---------------------------------------------------------------- Pckd (ARC methods 2, 3, 4, 8) */

/* Decoder state. The original keeps it in D:1FA8-D:1FBE and D:23EC; nothing else reads those. */
static u32 out_count, unpacked_len, packed_left;
static APTR pack_in, pack_out;
static int rle_state;
static u8 rle_last;

static int pack_getc(void)                              /* 0x14ED6 */
{
    if (!packed_left) return -1;
    packed_left--;
    return rd8(pack_in++);
}

static void pack_putc(u8 c)                             /* 0x14EF2 */
{
    out_count++;
    /* README bug 3: the original's bge skips the last byte, which stays 0 (MEMF_CLEAR). */
    if (g_original_bugs ? (s32)out_count < (s32)unpacked_len : (s32)out_count <= (s32)unpacked_len)
        wr8(pack_out++, c);
}

static void rle90_out(u8 c)                             /* 0x14E6C */
{
    if (rle_state == 0) {
        if (c == 0x90) rle_state = 1;
        else { rle_last = c; pack_putc(c); }
    } else {
        if (c == 0) pack_putc(0x90);
        else while (--c) pack_putc(rle_last);
        rle_state = 0;
    }
}

/* Method 4, "squeezed": u16 node count (little-endian), nodes of two s16 children (negative = leaf
 * -(byte + 1), 256 = end), bits LSB first. 0x14D64 / 0x14DC6 / 0x14DE0. */
static s16 huff_nodes[257][2];
static int huff_bitpos, huff_cur;

static int huff_read_word(void)
{
    int lo = pack_getc(), hi = pack_getc();
    return (s16)(u16)((lo & 0xFF) | (hi & 0xFF) << 8);
}

static void huff_read_tree(void)
{
    int n = (u16)huff_read_word();
    if (n > 256) n = 256;
    for (int k = 0; k < n; k++) {
        huff_nodes[k][0] = (s16)huff_read_word();
        huff_nodes[k][1] = (s16)huff_read_word();
    }
    if (n == 0) huff_nodes[0][0] = huff_nodes[0][1] = -257;
    huff_bitpos = 8;
}

static int huff_decode(void)
{
    int i = 0;
    while (i >= 0) {
        if (huff_bitpos > 7) {
            int c = pack_getc();
            if (c < 0) return -1;
            huff_cur = c;
            huff_bitpos = 0;
        }
        i = huff_nodes[i][huff_cur & 1];
        huff_cur >>= 1;
        huff_bitpos++;
    }
    i = -(i + 1);
    return i == 256 ? -1 : i;
}

/* Method 8, "crunched": Unix compress 4.0 as ported in the DOS and Amiga executables (codes are read n_bits
 * bytes at a time). 0x14F0E / 0x1506C. */
static int lzw_maxbits, lzw_n_bits, lzw_maxcode, lzw_maxmaxcode, lzw_free_ent, lzw_clear_flg, lzw_offset, lzw_size;
static u8 lzw_buf[20];
static u16 *lzw_prefix;
static u8 *lzw_suffix;

static int lzw_getcode(void)
{
    if (lzw_clear_flg > 0 || lzw_offset >= lzw_size || lzw_free_ent > lzw_maxcode) {
        if (lzw_free_ent > lzw_maxcode) {
            lzw_n_bits++;
            lzw_maxcode = lzw_n_bits == lzw_maxbits ? lzw_maxmaxcode : (1 << lzw_n_bits) - 1;
        }
        if (lzw_clear_flg > 0) {
            lzw_n_bits = 9;
            lzw_maxcode = 511;
            lzw_clear_flg = 0;
        }
        int got = 0;
        while (got < lzw_n_bits) {
            int c = pack_getc();
            if (c < 0) break;
            lzw_buf[got++] = (u8)c;
        }
        if (got == 0) return -1;
        memset(lzw_buf + got, 0, sizeof lzw_buf - (size_t)got);
        lzw_offset = 0;
        lzw_size = (got << 3) - (lzw_n_bits - 1);
    }
    int byte = lzw_offset >> 3;
    u32 v = lzw_buf[byte] | (u32)lzw_buf[byte + 1] << 8 | (u32)lzw_buf[byte + 2] << 16;
    int code = (int)((v >> (lzw_offset & 7)) & ((1u << lzw_n_bits) - 1));
    lzw_offset += lzw_n_bits;
    return code;
}

static void lzw_decompress(void)
{
    lzw_maxbits = pack_getc();
    if (lzw_maxbits < 9 || lzw_maxbits > 16) lzw_maxbits = 12;
    lzw_maxmaxcode = 1 << lzw_maxbits;
    lzw_n_bits = 9;
    lzw_maxcode = 511;
    lzw_clear_flg = 0;
    lzw_offset = lzw_size = 0;
    lzw_free_ent = 257;
    lzw_prefix = calloc((size_t)lzw_maxmaxcode, sizeof *lzw_prefix);
    lzw_suffix = calloc((size_t)lzw_maxmaxcode, 1);
    u8 *stack = malloc((size_t)lzw_maxmaxcode + 1);
    for (int i = 0; i < 256; i++) lzw_suffix[i] = (u8)i;

    int oldcode = lzw_getcode(), finchar = oldcode;
    if (oldcode >= 0) {
        rle90_out((u8)finchar);
        for (;;) {
            int code = lzw_getcode();
            if (code < 0) break;
            if (code == 256) {
                lzw_clear_flg = 1;
                lzw_free_ent = 256;
                code = lzw_getcode();
                if (code < 0) break;
            }
            int incode = code, sp = 0;
            if (code >= lzw_free_ent) {
                stack[sp++] = (u8)finchar;
                code = oldcode;
            }
            while (code >= 256 && sp < lzw_maxmaxcode) {
                stack[sp++] = lzw_suffix[code];
                code = lzw_prefix[code];
            }
            finchar = lzw_suffix[code & 0xFF];
            stack[sp++] = (u8)finchar;
            while (sp > 0) rle90_out(stack[--sp]);
            if (lzw_free_ent < lzw_maxmaxcode) {
                lzw_prefix[lzw_free_ent] = (u16)oldcode;
                lzw_suffix[lzw_free_ent] = (u8)finchar;
                lzw_free_ent++;
            }
            oldcode = incode;
        }
    }
    free(stack);
    free(lzw_prefix);
    free(lzw_suffix);
}

static void pckd_unpack(APTR p, APTR out)               /* 0x14C9C */
{
    out_count = 0;
    packed_left = rd32(p + 4);
    unpacked_len = rd32(p + 8);
    pack_in = p + 16;
    pack_out = out;
    rle_state = 0;
    int c;
    switch (rd16(p + 0xC)) {
    case 2: while ((c = pack_getc()) != -1) pack_putc((u8)c); break;
    case 3: while ((c = pack_getc()) != -1) rle90_out((u8)c); break;
    case 4: huff_read_tree(); while ((c = huff_decode()) != -1) rle90_out((u8)c); break;
    case 8: lzw_decompress(); break;
    default: break;                                     /* output stays zero (MEMF_CLEAR) */
    }
}

/* ---------------------------------------------------------------- load_file */

APTR load_file(const char *name)      { return load_file_flags(name, 0x10001); }
APTR load_file_chip(const char *name) { return load_file_flags(name, 0x10003); }

APTR load_file_flags(const char *name, u32 flags)
{
    SETD32(D_g_loadedBuf, 0);
    SETD32(D_g_loadedSize, 0);
    u32 size = 0;
    u8 *data = adisk_read(name, &size);
    if (!data) return 0;
    APTR result = 0;
    u32 result_size = 0;
    if (size >= 16) {                                   /* the original fails on files shorter than 16 bytes */
        if (memcmp(data, "Pckd", 4) == 0) {
            u32 ulen = (u32)data[8] << 24 | (u32)data[9] << 16 | (u32)data[10] << 8 | data[11];
            APTR out = mem_alloc(ulen, flags);
            APTR tmp = out ? alloc_public(size) : 0;
            if (tmp) {
                amem_put(tmp, data, size);
                pckd_unpack(tmp, out);                  /* no size or CRC check, as in the original */
                free_mem(tmp);
                result = out;
                result_size = ulen;
            } else if (out) {
                free_mem(out);
            }
        } else {
            APTR buf = mem_alloc(size, flags);
            if (buf) {
                amem_put(buf, data, size);
                result = buf;
                result_size = size;
            }
        }
    }
    free(data);
    SETD32(D_g_loadedBuf, result);
    SETD32(D_g_loadedSize, result_size);
    return result;
}
