/* Timing and input — port of td 0x1180E-0x11886 (Ticks VBLInt), 0x153CC-0x1543E (joystick), 0x154E4-0x15718
 * (input.device handler, keys) (port/amiga/spec/platform_video.md §4.8-§4.10). */
#include "platform.h"

#include "../ahost.h"
#include "../asymbols.h"

void (*vbl_gate_select)(u16 gear);

/* ---------------------------------------------------------------- joystick (JOY1DAT, CIA-A PRA bit 7) */

/* JOY1DAT as the hardware reports the port-2 stick: right = bit 1, left = bit 9, down = bit 1 ^ bit 0,
 * up = bit 9 ^ bit 8. */
static u16 joy1dat(bool *fire)
{
    bool up, down, left, right;
    ahost_joystick(&up, &down, &left, &right, fire);
    u16 v = 0;
    if (right) v |= 0x0002;
    if (left) v |= 0x0200;
    if (down != right) v |= 0x0001;
    if (up != left) v |= 0x0100;
    return v;
}

s16 read_fire(void)
{
    bool fire;
    joy1dat(&fire);
    return fire ? 1 : 0;
}

/* 0x15416 uses its own copy of the direction table at 0x1543E; 0x11886 uses D:03DC. Same contents. */
static s16 joy_code(APTR table)
{
    bool fire;
    u16 j = joy1dat(&fire);
    return rd8(table + ((j & 3) | ((j >> 6) & 0xC)));
}

s16 read_joy(void) { return joy_code(0x1543E); }
s16 joy_fire(void) { s16 f = read_fire(); SETD16(D_g_joyFire, f); return f; }
s16 joy_dir(void)  { s16 d = read_joy();  SETD16(D_g_joyDir, d);  return d; }
void fire_port_init(void) {}                            /* CIA-A DDRA bit 7 = input: nothing to emulate */

/* ---------------------------------------------------------------- Ticks VBLInt */

static void vbl_server(void)                            /* 0x118B2, priority -80 */
{
    SETD32(D_tick_count, D32(D_tick_count) + 1);
    if (!read_fire()) {
        if (D16(D_gearbox_hide_delay) == 0 && D16(D_gearbox_always) == 0) SETD16(D_gearbox_show, 0);
    } else if (D16(D_gate_mode) == 1) {                 /* O mode: joystick + fire selects the gear */
        SETD16(D_gearbox_show, 0xFFFF);
        s16 dir = joy_code(DADDR(D_joy_dir_table)), old = (s16)D16(D_vbl_last_dir);
        SETD16(D_vbl_last_dir, dir);
        if ((old != dir || D16(D_knob_target_y) != D16(D_knob_y) || D16(D_knob_target_x) != D16(D_knob_x)) && dir != 0) {
            u16 i = (u16)((dir << 4) + (s16)D16(D_gate_row) * 4 + (s16)D16(D_gate_col));
            if (vbl_gate_select) vbl_gate_select(rd8(D32(D_car_gate_next) + i));
        }
    }
}

void vbl_install(void) { ahost_add_vbl_server(-80, vbl_server); }
void vbl_remove(void)  { ahost_remove_vbl_server(vbl_server); }

/* ---------------------------------------------------------------- keys */

static void input_handler(u8 code, u16 qual)            /* 0x15698 */
{
    if (code & 0x80) return;                            /* key up: swallowed */
    if (qual & D16(D_key_pass_qual)) return;            /* passed to the system (nothing there in the port) */
    s16 n = (s16)D16(D_key_count);
    if (n < (s16)D16(D_key_max)) {
        SETD8(D_key_code + n, code);
        SETD16(D_key_qual + 2 * n, qual);
        SETD16(D_key_count, n + 1);
    }
}

void input_init(u16 passmask)
{
    SETD16(D_key_pass_qual, passmask);
    SETD16(D_key_max, 0xA);
    SETD16(D_key_count, 0);
    ahost_set_key_handler(input_handler);
}

void input_shutdown(void) { ahost_set_key_handler(0); }

s16 key_available(void)
{
    ahost_pump();                                       /* the handler runs asynchronously on the Amiga */
    return D16(D_key_count) ? -1 : 0;
}

u32 get_key(void)
{
    while (!key_available()) ahost_wait_tof();
    u32 k = (u32)D16(D_key_qual) << 16 | D8(D_key_code);
    s16 n = (s16)(D16(D_key_count) - 1);
    SETD16(D_key_count, n);
    for (s16 i = 0; i <= n; i++) {                      /* moves one entry past the queue, as the original */
        SETD8(D_key_code + i, D8(D_key_code + i + 1));
        SETD16(D_key_qual + 2 * i, D16(D_key_qual + 2 * i + 2));
    }
    return k;
}

/* console.device RawKeyConvert with the default USA keymap, into a 1-byte buffer: the character, or 0 when
 * the key gives none or more than one (cursor and function keys give CSI sequences). Alt is not mapped: the
 * USA keymap's Alt characters (Latin-1) would come out as the plain ones. */
static u8 raw_key_convert(u8 code, u16 qual)
{
    static const char plain[0x41] = "`1234567890-=\\\0" "0"
                                    "qwertyuiop[]\0" "123"
                                    "asdfghjkl;'\0\0" "456"
                                    "\0zxcvbnm,./\0" ".789";
    static const char shifted[0x41] = "~!@#$%^&*()_+|\0" "0"
                                      "QWERTYUIOP{}\0" "123"
                                      "ASDFGHJKL:\"\0\0" "456"
                                      "\0ZXCVBNM<>?\0" ".789";
    if (code & 0x80) return 0;
    if (code < 0x40) {
        bool keypad = code == 0x0F || (code >= 0x1D && code <= 0x1F) || (code >= 0x2D && code <= 0x2F) || code >= 0x3C;
        char c = keypad ? plain[code] : (qual & (IEQUALIFIER_LSHIFT | IEQUALIFIER_RSHIFT)) ? shifted[code] : plain[code];
        if (!c) return 0;
        if ((qual & IEQUALIFIER_CAPSLOCK) && c >= 'a' && c <= 'z') c = (char)(c - 0x20);
        if ((qual & IEQUALIFIER_CONTROL) && c >= 0x40) c = (char)(c & 0x1F);
        return (u8)c;
    }
    switch (code) {
    case 0x40: return ' ';
    case 0x41: return 0x08;
    case 0x42: return 0x09;
    case 0x43: case 0x44: return 0x0D;
    case 0x45: return 0x1B;
    case 0x46: return 0x7F;
    case 0x4A: return '-';
    case 0x5A: return '(';
    case 0x5B: return ')';
    case 0x5C: return '/';
    case 0x5D: return '*';
    case 0x5E: return '+';
    default: return 0;
    }
}

s16 key_to_ascii(u32 k) { return raw_key_convert((u8)k, (u16)(k >> 16)); }
s16 get_char(void) { return key_to_ascii(get_key()); }
