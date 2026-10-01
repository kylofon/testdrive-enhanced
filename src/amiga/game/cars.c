/* Cars.txt — port of td 0x10346 load_cars_txt and 0x10402 skip_line (port/amiga/spec/game_flow.md §4), plus two
 * small C-runtime helpers the game code needs on strings in amem[]. */
#include "game.h"

#include "../aport.h"
#include "../asymbols.h"
#include "../platform/platform.h"

/* 0x10402: advance *pp past the next '\n' (stops on the NUL); returns the new *pp. */
APTR skip_line(APTR *pp)
{
    while (rd8(*pp) != 0 && rd8(*pp) != '\n') (*pp)++;
    if (rd8(*pp) != 0) (*pp)++;
    return *pp;
}

/* 0x10346. load = 0 only clears the count. The previous buffer is never freed (D:0352 is always 0), as in the
 * original: it is dropped here and freed by free_mem(-1) at exit. */
void load_cars_txt(s16 load)
{
    if (D16(D_g_carsKeepBuf) == 0) SETD32(D_g_carsTxtPtr, 0);
    SETD16(D_g_numCars, 0);
    if (D32(D_g_carsTxtPtr)) free_mem(D32(D_g_carsTxtPtr));   /* dead code, see above */
    if (!load) return;
    SETD32(D_g_carsTxtPtr, load_file("Cars.Txt"));
    if (!D32(D_g_carsTxtPtr)) {
        SETD32(D_g_carsTxtPtr, 0);
        SETD16(D_g_fatalInit, 1);
        return;
    }
    s16 n = 0;                                          /* the original's is uninitialised without a number */
    amem_scan_ints(D32(D_g_carsTxtPtr), &n, 1);
    APTR p = D32(D_g_carsTxtPtr);
    if (n > 0 && n < 0x1E) {                            /* the count line is skipped only for 1..29 */
        skip_line(&p);
        SETD32(D_g_carsTxtPtr, p);
    }
    /* n is not clamped: n >= 30 would run over g_carNames[] as in the original (the shipped file has 5) */
    for (s16 i = 0; i < n; i++) {
        s16 k = DS16(D_g_numCars);
        SETD16(D_g_numCars, k + 1);
        SETD32(D_g_carNames + 4 * (u32)(u16)k, p);
        APTR next = skip_line(&p);
        SETD32(D_g_carsTxtPtr, p);
        /* Bug 11 (README): the original always zeroes next[-1], which is the last character of a last line
         * without '\n' (skip_line stopped on the NUL). Fixed: only a '\n' is zeroed. */
        if (g_original_bugs || rd8(next - 1) == '\n') wr8(next - 1, 0);
    }
}

/* Aztec sscanf with "%d" conversions separated by white space: skip white space, optional sign, decimal digits
 * (16-bit int). Stops at the first conversion that finds no digit. */
int amem_scan_ints(APTR s, s16 *vals, int n)
{
    if (!s) return 0;
    int done = 0;
    while (done < n) {
        u8 c = rd8(s);
        while (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v') c = rd8(++s);
        bool neg = false;
        if (c == '-' || c == '+') {
            neg = c == '-';
            c = rd8(++s);
        }
        if (c < '0' || c > '9') break;
        s16 v = 0;
        while (c >= '0' && c <= '9') {
            v = (s16)(v * 10 + (c - '0'));
            c = rd8(++s);
        }
        vals[done++] = neg ? (s16)-v : v;
    }
    return done;
}

void amem_cstr(APTR s, char *dst, size_t n)
{
    size_t i = 0;
    if (n == 0) return;
    if (s)
        for (; i + 1 < n; i++) {
            u8 c = rd8(s + (u32)i);
            if (!c) break;
            dst[i] = (char)c;
        }
    dst[i] = 0;
}
