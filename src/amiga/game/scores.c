/* High scores — port of td 0x12FF0-0x13BD8 and 0x13E86-0x140B6 (port/amiga/spec/game_flow.md §4): the
 * HighScores file with its CRC-16, the name entry over the car's logo picture, the score table with its
 * per-row copper palettes, and the credits page. `high_scores` 0x12F28, which calls all of these, is in
 * game/main.c; the text RastPort D:2876 (topaz 8 on bitmap A) is set up there by text_init 0x13250.
 *
 * All timing is in vertical blanks (tick_count D:03D8, 60 Hz NTSC, port/amiga/README.md *Decisions* 1), so the
 * spec's 900 VBL are 15 s and the name entry's 6000 VBL are 100 s. The original's busy waits pump the host
 * here (the VBL server only runs from ahost_pump; key_available and gfx_WaitTOF pump too).
 *
 * Aztec C `int` is 16 bits, so every count and index is s16. Strings the original keeps on its stack live in
 * host buffers; the ones it passes to read_line or strcpy are records in amem[] (names/cars are char[8][40],
 * scores long[8], allocated by high_scores). */
#include "game.h"

#include <stdio.h>
#include <string.h>

#include "../adisk.h"
#include "../agfx.h"
#include "../ahost.h"
#include "../asymbols.h"
#include "../platform/blit.h"
#include "../platform/platform.h"

#define ID(a, b, c, d) ((u32)(a) << 24 | (u32)(b) << 16 | (u32)(c) << 8 | (u32)(d))

#define VIEW_A   DADDR(D_g_viewA)                        /* D:293E */
#define VIEW_B   DADDR(D_g_viewB)                        /* D:2950 */
#define RP()     D32(D_g_tmpRastPort)                    /* D:2876 */

#define RP_TxBaseline      0x3E                          /* struct RastPort, graphics.library 1.3 */

#define HS_ENTRIES   8
#define HS_RECORD    0x28                                /* 40 bytes per name / car path */
#define HS_CRC_INIT  0x6D62
#define HS_CRC_POLY  0x2058

static u32 ticks(void) { return D32(D_tick_count); }

static APTR vp_of(APTR view) { return rd32(view + VIEW_ViewPort); }

/* &BitMap.Planes[0] of a View's first ViewPort: the blit_set_dest argument (0x13EAE walks View -> ViewPort ->
 * RasInfo -> BitMap). */
static APTR view_planes(APTR view)
{
    return rd32(rd32(vp_of(view) + VP_RasInfo) + RI_BitMap) + BM_Planes;
}

/* strcpy 0x169E0 between two records in amem[], and its host-string counterpart. */
static void amem_strcpy(APTR dst, APTR src)
{
    u8 c;
    do { c = rd8(src++); wr8(dst++, c); } while (c);
}

static void amem_put_cstr(APTR dst, const char *s)
{
    do { wr8(dst++, (u8)*s); } while (*s++);
}

static u32 amem_strlen(APTR s)                           /* strlen 0x16F80 */
{
    u32 n = 0;
    while (rd8(s + n)) n++;
    return n;
}

/* The car path of car `idx`: g_carNames[idx] (D:2E66) points into the Cars.txt buffer. */
static void car_path(s16 idx, char *dst, size_t n)
{
    amem_cstr(rd32(DADDR(D_g_carNames) + 4u * (u32)(u16)idx), dst, n);
}

/* Aztec `sscanf(s, "%ld", &v)`: white space, an optional sign, decimal digits. Returns 0 when there is no
 * number, and then leaves *v alone (as sscanf does: the original's local keeps its previous value). */
static int scan_long(APTR s, s32 *v)
{
    u8 c = rd8(s);
    while (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v') c = rd8(++s);
    bool neg = false;
    if (c == '-' || c == '+') {
        neg = c == '-';
        c = rd8(++s);
    }
    if (c < '0' || c > '9') return 0;
    s32 n = 0;
    while (c >= '0' && c <= '9') {
        n = n * 10 + (c - '0');
        c = rd8(++s);
    }
    *v = neg ? -n : n;
    return 1;
}

/* Aztec `sscanf(s, "%x", &v)` into a 16-bit int: white space, an optional sign, an optional 0x, hex digits. */
static int scan_hex(APTR s, u16 *v)
{
    u8 c = rd8(s);
    while (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v') c = rd8(++s);
    bool neg = false;
    if (c == '-' || c == '+') {
        neg = c == '-';
        c = rd8(++s);
    }
    if (c == '0' && (rd8(s + 1) == 'x' || rd8(s + 1) == 'X')) {
        s += 2;
        c = rd8(s);
    }
    int digit = -1;
    u16 n = 0;
    for (;;) {
        if (c >= '0' && c <= '9') digit = c - '0';
        else if (c >= 'a' && c <= 'f') digit = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') digit = c - 'A' + 10;
        else break;
        n = (u16)(n * 16 + digit);
        c = rd8(++s);
    }
    if (digit < 0) return 0;
    *v = neg ? (u16)-n : n;
    return 1;
}

/* ================================================================ the HighScores file */

/* 0x12FF0: copy bytes >= 0x20 (signed, so >= 0x80 ends the line too) from src to dst, NUL-terminate, skip one
 * terminator. No length limit, as in the original: a line longer than 39 bytes overruns its record. */
static APTR read_line(APTR dst, APTR src)
{
    if (rd8(src)) {
        while ((s8)rd8(src) >= 0x20) wr8(dst++, rd8(src++));
        if (rd8(src)) src++;
    }
    wr8(dst, 0);
    return src;
}

/* 0x1406E -> 0x14088 (asm): MSB-first CRC-16 over len/2 big-endian words; an odd trailing byte is dropped and
 * `len` is used as 16 bits (lsr.w). */
static u16 crc16(APTR buf, s32 len, u16 crc, u16 poly)
{
    for (u16 n = (u16)((u16)len >> 1); n != 0; n--) {
        u16 d = rd16(buf);
        buf += 2;
        for (int b = 0; b < 16; b++) {
            crc ^= (u16)(d & 0x8000);
            d = (u16)(d << 1);
            u16 carry = (u16)(crc & 0x8000);
            crc = (u16)(crc << 1);
            if (carry) crc ^= poly;
        }
    }
    return crc;
}

/* 0x13028: parse HighScores (8 x name/score/car path, then the CRC as "%04x"), check the CRC over exactly the
 * bytes read. A missing or bad file clears all 8 entries and returns 1, so high_scores rewrites it. */
s16 scores_load(APTR names, APTR scores, APTR cars)
{
    s16 reset = 0;                                       /* d4 */
    APTR tmp = exec_AllocMem(0x14, MEMF_PUBLIC | MEMF_CLEAR);   /* the original's stack local tmp[20] */
    if (!tmp) ahost_fatal("out of memory (high scores)");
    if (load_file("HighScores")) {                       /* the buffer is also in D:24BC */
        APTR buf = D32(D_g_loadedBuf), p = buf;
        for (s16 i = 0; i < HS_ENTRIES; i++) {
            p = read_line(names + HS_RECORD * (u32)(u16)i, p);
            p = read_line(tmp, p);
            p = read_line(cars + HS_RECORD * (u32)(u16)i, p);
            s32 v = (s32)rd32(scores + 4u * (u32)(u16)i);
            scan_long(tmp, &v);                          /* sscanf("%ld"): unchanged when there is no number */
            wr32(scores + 4u * (u32)(u16)i, (u32)v);
        }
        u16 crc = crc16(buf, (s32)(p - buf), HS_CRC_INIT, HS_CRC_POLY);
        u16 crcFile = 0;                                 /* the original's local is uninitialised */
        scan_hex(p, &crcFile);
        free_mem(D32(D_g_loadedBuf));
        if (crc == crcFile) {
            exec_FreeMem(tmp, 0x14);
            return reset;                                /* 0 */
        }
    }
    reset = 1;
    for (s16 i = 0; i < HS_ENTRIES; i++) {
        wr8(names + HS_RECORD * (u32)(u16)i, 0);
        wr32(scores + 4u * (u32)(u16)i, 0);
        wr8(cars + HS_RECORD * (u32)(u16)i, 0);
    }
    exec_FreeMem(tmp, 0x14);
    return reset;
}

/* 0x1313E: format the 8 entries into a 500-byte buffer, then write the buffer and "%04x\n" of its CRC.
 * The original uses fopen("HighScores", "w") / fprintf / fclose on the game disk; the port writes through
 * adisk_write, which puts the file beside a read-only .adf (adisk.h). */
void scores_save(APTR names, APTR scores, APTR cars)
{
    APTR buf = alloc_public(0x1F4);                      /* 500 bytes, not bounds-checked in the original */
    if (!buf) return;
    APTR p = buf;
    char name[0x80], car[0x80], line[0x140];             /* records are 40 bytes; longer strings truncate */
    for (s16 i = 0; i < HS_ENTRIES; i++) {
        amem_cstr(names + HS_RECORD * (u32)(u16)i, name, sizeof name);
        amem_cstr(cars + HS_RECORD * (u32)(u16)i, car, sizeof car);
        s32 score = (s32)rd32(scores + 4u * (u32)(u16)i);
        int n = snprintf(line, sizeof line, "%s\n%ld\n%s\n", name, (long)score, car);
        if (n < 0) n = 0;
        if ((u32)n > sizeof line - 1) n = (int)sizeof line - 1;
        /* sprintf(p, ...) — the port stops at the end of the 500-byte block instead of overrunning it */
        if (p + (u32)n + 1 > buf + 0x1F4) n = (int)(buf + 0x1F4 - 1 - p);
        amem_put(p, line, (u32)n + 1);                   /* with the NUL sprintf writes */
        p = buf + amem_strlen(buf);                      /* p = buf + strlen(buf), as the original recomputes */
    }
    u32 len = (u32)(p - buf);
    u16 crc = crc16(buf, (s32)len, HS_CRC_INIT, HS_CRC_POLY);
    u8 file[0x1F4 + 8];
    amem_get(file, buf, len);
    int tail = snprintf((char *)file + len, sizeof file - len, "%04x\n", (unsigned)crc);
    adisk_write("HighScores", file, len + (u32)(tail > 0 ? tail : 0));
    free_mem(buf);
}

/* ================================================================ name entry */

/* 0x135B4: the name editor. Printable 0x20..0x7E (signed byte compares) up to maxlen characters, Backspace,
 * Return; `xs[]` keeps the pen x after every character so Backspace can erase exactly one. A 6000-VBL idle
 * timeout (100 s, reset by every key) or Ctrl-C leaves the name empty. The original polls without waiting;
 * key_available pumps the host here, so the window stays alive. `buf` is a host buffer of maxlen + 1 bytes. */
static void text_input_line(char *buf, s16 maxlen)
{
    u32 last = ticks();
    s16 n = 0;
    s16 xs[100];
    xs[0] = (s16)rd16(RP() + RP_cp_x);
    text_cursor();                                       /* 0x134F2: cursor on */
    for (;;) {
        if (key_available()) {                           /* 0x156EC */
            last = ticks();
            s8 c = (s8)get_char();                       /* 0x15628: ASCII through the USA keymap */
            if (c == 0x0D) {
                buf[n] = 0;
                text_cursor();
                return;
            }
            if (c == 0x08) {
                if (n != 0) {
                    text_cursor();
                    n = (s16)(n - 1);
                    s16 cy = (s16)rd16(RP() + RP_cp_y);
                    erase_rect(xs[n], (s16)(cy - 8), xs[n + 1], (s16)(cy + 1));   /* 0x1343A */
                    text_move(xs[n], (s16)rd16(RP() + RP_cp_y));
                    text_cursor();
                }
            } else if (c >= 0x20 && c < 0x7F && n < maxlen) {
                buf[n] = (char)c;
                n = (s16)(n + 1);
                char s[2];
                s[0] = (char)(c & 0x7F);
                s[1] = 0;
                text_cursor();
                text_print(s);                           /* 0x13306 */
                xs[n] = (s16)rd16(RP() + RP_cp_x);
                text_cursor();
            }
        }
        if (last + 0x1770 <= ticks() || quit_requested()) break;   /* 6000 VBL idle, or Ctrl-C */
    }
    buf[0] = 0;                                          /* timeout / Ctrl-C: no entry */
    text_cursor();
}

/* The release wait after a fire press (0x13928, 0x13DF0, 0x13BB0): Delay(3), spin while the button is held,
 * Delay(5). The spin pumps the host. */
static void wait_fire_release(void)
{
    dos_Delay(3);
    while (joy_fire()) ahost_pump();
    dos_Delay(5);
}

/* 0x13708: the full-screen `<car>logo` picture, the prompt, the 17-character name entry and the insertion into
 * the table. A timeout or an empty name inserts nothing. */
void scores_enter_name(s32 score, s16 car, APTR names, APTR scores, APTR cars)
{
    char stem[64], path[80], name[0x12];                 /* the original's name[48], filled to 17 + NUL */

    view_clear(VIEW_B);                                  /* 0x1574E */
    show_view(VIEW_B);                                   /* black while A is loaded */
    car_path(car, stem, sizeof stem);
    snprintf(path, sizeof path, "%slogo", stem);         /* e.g. "cars/P911tlogo" */
    if (load_file(path))
        ilbm_to_view(D32(D_g_loadedBuf), VIEW_A);        /* 0x1063A */
    else
        view_clear(VIEW_A);
    if (D32(D_g_loadedBuf)) free_mem(D32(D_g_loadedBuf));   /* the original leaks it (spec, open question 9) */
    show_view(VIEW_A);

    s16 i = 0;                                           /* reads scores[i] before testing i, as the original */
    while (!(score > (s32)rd32(scores + 4u * (u32)(u16)i)) && i < HS_ENTRIES) i = (s16)(i + 1);
    if (i >= HS_ENTRIES) return;                         /* unreachable: high_scores tests score > scores[7] */

    text_move(0x28, 0x96);
    text_print("You have qualified as one");
    text_move(0x1E, 0xA0);
    text_print("of Test Drive's best drivers.");
    text_move(0x19, 0xB4);
    text_print("Enter your name:");
    s16 bx = (s16)(rd16(RP() + RP_cp_x) + 2);            /* 25 + 16*8 + 2 = 155 with topaz 8 */
    draw_box_outline(bx, 0xAA, (s16)(bx + 0x96), 0xB7);  /* 0x1336E */
    while (key_available()) get_char();                  /* flush the keys typed during the drive */
    text_move((s16)(bx + 3), 0xB4);
    text_input_line(name, 0x11);                         /* up to 17 characters */
    if (name[0] == 0) return;                            /* empty name / timeout: no entry */

    for (s16 j = 6; j >= i; j = (s16)(j - 1)) {          /* push entries i..6 down one place */
        amem_strcpy(names + HS_RECORD * (u32)(u16)(j + 1), names + HS_RECORD * (u32)(u16)j);
        wr32(scores + 4u * (u32)(u16)(j + 1), rd32(scores + 4u * (u32)(u16)j));
        amem_strcpy(cars + HS_RECORD * (u32)(u16)(j + 1), cars + HS_RECORD * (u32)(u16)j);
    }
    amem_put_cstr(names + HS_RECORD * (u32)(u16)i, name);
    wr32(scores + 4u * (u32)(u16)i, (u32)score);
    amem_strcpy(cars + HS_RECORD * (u32)(u16)i, rd32(DADDR(D_g_carNames) + 4u * (u32)(u16)car));

    u32 t = ticks() + 0x3C;                              /* 60 VBL = 1 s to read the name back */
    do {
        poll_input();
        if (quit_requested() || joy_fire()) return;
        gfx_WaitTOF();
    } while (t > ticks());
    wait_fire_release();
}

/* ================================================================ the table */

/* 0x13E86: blit the `logo` shape of `<car>logo.Shp` into view A at (0x10, row * 0x23 + 3). */
static void hs_draw_logo(s16 row, APTR carPath)
{
    char stem[64], path[80];
    amem_cstr(carPath, stem, sizeof stem);
    snprintf(path, sizeof path, "%slogo.Shp", stem);
    if (!load_file_chip(path)) return;
    blit_set_dest(view_planes(VIEW_A));                  /* 0x10E26 */
    set_clip_full();                                     /* 0x10FA4 */
    APTR s = find_shape(D32(D_g_loadedBuf), ID('l', 'o', 'g', 'o'));   /* 0x15472 */
    if (s) {
        blit_wait();                                     /* WaitBlit */
        gfx_OwnBlitter();
        SETD32(D_g_blitTag, ID('l', 'o', 'g', 'o'));
        blit_shape(s, 0, 0x10, (s16)(row * 0x23 + 3));   /* 0x10E58 */
        blit_wait();                                     /* 0x10E78 */
        gfx_DisownBlitter();
    }
    free_mem(D32(D_g_loadedBuf));
}

/* 0x13F32: row -1 loads the default palette D:098E into view A's ViewPort; otherwise append to the user copper
 * list a WAIT for line row * 0x23 + 1 and 32 MOVEs to COLOR00..COLOR31 from `<car>Logo.Pal` (one 12-bit hex
 * value per line), or from the default palette when the file is missing. */
static void hs_logo_palette(s16 row, APTR carPath, APTR ucl)
{
    if (row == -1) {
        gfx_LoadRGB4(vp_of(VIEW_A), DADDR(D_HS_DEFAULT_PALETTE), 0x20);
        return;
    }
    gfx_CWait(ucl, (s16)(row * 0x23 + 1), 0);
    gfx_CBump(ucl);

    char stem[64], path[80];
    amem_cstr(carPath, stem, sizeof stem);
    snprintf(path, sizeof path, "%sLogo.Pal", stem);
    APTR p = load_file(path);
    if (p) {
        APTR line = exec_AllocMem(0xC, MEMF_PUBLIC | MEMF_CLEAR);   /* the original's stack local line[12] */
        if (!line) ahost_fatal("out of memory (logo palette)");
        u16 v = 0;                                       /* the original's local is uninitialised */
        for (s16 k = 0; k < 0x20; k++) {
            p = read_line(line, p);
            scan_hex(line, &v);                          /* unchanged when the line has no hex digits */
            gfx_CMove(ucl, (u32)(0xDFF000 + CUSTOM_COLOR00 + 2 * k), v);
            gfx_CBump(ucl);
        }
        exec_FreeMem(line, 0xC);
        free_mem(D32(D_g_loadedBuf));
    } else {
        for (s16 k = 0; k < 0x20; k++) {
            gfx_CMove(ucl, (u32)(0xDFF000 + CUSTOM_COLOR00 + 2 * k), D16(D_HS_DEFAULT_PALETTE + 2 * k));
            gfx_CBump(ucl);
        }
    }
}

/* The wait shared by the table and the credits (0x13DC8, 0x13B90): up to 900 VBL, ended early by fire or
 * Ctrl-C. Returns 0 when it was ended, -1 on the timeout. */
static s16 wait_or_timeout(u32 vbl)
{
    u32 t = ticks() + vbl;
    for (;;) {
        poll_input();                                    /* 0x10462 */
        if (quit_requested() || joy_fire()) return 0;
        gfx_WaitTOF();
        if (t <= ticks()) return -1;
    }
}

/* 0x13BD8: "Test Drive Top Scores". The top 4 rows carry their car's logo and switch the whole 32-colour
 * palette at their own line through view A's user copper list; rows 4..7 are plain text in the last palette.
 * Rows with score 0 are skipped entirely (they add no copper entries either). */
s16 scores_show(APTR names, APTR scores, APTR cars)
{
    char buf[64], name[HS_RECORD];                       /* the original's buffer is 50 bytes */
    s16 r = 0;
    APTR ucl = exec_AllocMem(UCL_SIZE, MEMF_PUBLIC | MEMF_CLEAR);
    if (!ucl) ahost_fatal("out of memory (high-score copper list)");
    view_clear(VIEW_A);
    view_clear(VIEW_B);
    hs_logo_palette(-1, 0, 0);                           /* the default palette into view A */
    show_view(VIEW_B);                                   /* blank while A is drawn */
    text_move(0x5F, 9);
    text_print("Test Drive Top Scores");
    for (s16 i = 0; i < 4; i++) {
        if (rd32(scores + 4u * (u32)(u16)i) == 0) continue;
        hs_draw_logo(i, cars + HS_RECORD * (u32)(u16)i);
        hs_logo_palette(i, cars + HS_RECORD * (u32)(u16)i, ucl);
        text_move(0x5A, (s16)(i * 0x23 + 0x14));         /* y = 20, 55, 90, 125 */
        amem_cstr(names + HS_RECORD * (u32)(u16)i, name, sizeof name);
        snprintf(buf, sizeof buf, "%7ld  %s", (long)(s32)rd32(scores + 4u * (u32)(u16)i), name);
        text_print(buf);
    }
    gfx_CWait(ucl, 10000, 255);                          /* CEND */
    gfx_CBump(ucl);
    wr32(vp_of(VIEW_A) + VP_UCopIns, ucl);
    gfx_MakeVPort(VIEW_A, vp_of(VIEW_A));
    gfx_MrgCop(VIEW_A);
    for (s16 i = 4; i < HS_ENTRIES; i++) {
        if (rd32(scores + 4u * (u32)(u16)i) == 0) continue;
        text_move(0x5A, (s16)(i * 10 + 0x6B));           /* y = 147, 157, 167, 177 */
        amem_cstr(names + HS_RECORD * (u32)(u16)i, name, sizeof name);
        snprintf(buf, sizeof buf, "%7ld  %s", (long)(s32)rd32(scores + 4u * (u32)(u16)i), name);
        text_print(buf);
    }
    if ((s32)DS32(D_g_totalScore) > 0) {
        snprintf(buf, sizeof buf, "Your Score : %ld", (long)DS32(D_g_totalScore));
        text_move((s16)(0xA0 - (s16)(strlen(buf) * 4)), 0xC7);   /* centred: 4 px per topaz-8 half-character */
        text_print(buf);
    }
    show_view(VIEW_A);

    r = wait_or_timeout(0x384);                          /* 900 VBL = 15 s */
    wait_fire_release();
    gfx_WaitTOF();
    view_clear(VIEW_A);
    gfx_WaitTOF();
    gfx_FreeVPortCopLists(vp_of(VIEW_A));                /* frees ucl too */
    gfx_MakeVPort(VIEW_A, vp_of(VIEW_A));
    gfx_MrgCop(VIEW_A);
    return r;
}

/* ================================================================ credits */

/* 0x13A9E: the 24 lines of D:0926 centred in the pens of D:08F6 (1 headings, 11 names, 0 blank), drawn into
 * view A and dissolved onto the shown view B, then 900 VBL or fire. The first four lines are the crackers'
 * text; the original Amiga credits are not in this dump (README, *Credits*: deferred). */
s16 credits_show(void)
{
    s16 r = 0;
    view_clear(VIEW_A);
    view_clear(VIEW_B);
    show_view(VIEW_B);
    hs_logo_palette(-1, 0, 0);
    for (s16 i = 0; rd32(DADDR(D_CREDITS_TEXT) + 4u * (u32)(u16)i) != 0; i = (s16)(i + 1)) {
        char line[64];
        amem_cstr(rd32(DADDR(D_CREDITS_TEXT) + 4u * (u32)(u16)i), line, sizeof line);
        s16 w = (s16)gfx_TextLength(RP(), line, (u16)strlen(line));
        text_move((s16)((s16)(0x140 - w) / 2),           /* divs: signed */
                  (s16)(i * 8 + (s16)rd16(RP() + RP_TxBaseline) + 1));
        text_set_pen(DS16(D_CREDITS_PENS + 2 * (u32)(u16)i));
        text_print(line);
    }
    view_copy_palette(VIEW_A, VIEW_B);
    view_dissolve(VIEW_A, VIEW_B);                       /* 0x109D0: 8 passes, CPU-bound */
    r = wait_or_timeout(0x384);                          /* 900 VBL = 15 s */
    wait_fire_release();
    return r;
}
