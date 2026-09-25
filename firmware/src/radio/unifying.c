#include <string.h>

#include "unifying.h"

// Report type bytes in payload[1]
#define UNIFY_T_KEEPALIVE 0x40
#define UNIFY_T_KEY 0xC1
#define UNIFY_T_MOUSE 0xC2
#define UNIFY_T_MEDIA 0xC3
#define UNIFY_T_ENCRYPTED 0xD3

uint8_t unify_checksum(const uint8_t *payload, uint8_t len)
{
    if (!payload || len < 2)
        return 0;
    uint8_t sum = 0;
    for (uint8_t i = 0; i + 1 < len; i++)
        sum += payload[i];
    return (uint8_t)(0x00 - sum);
}

// HID usage codes to short names, the printable ones plus the keys an
// injection test actually uses. 0 when unknown.
static const struct
{
    uint8_t code;
    const char *name;
} c_key_names[] = {
    {0x04, "A"}, {0x05, "B"}, {0x06, "C"}, {0x07, "D"}, {0x08, "E"}, {0x09, "F"},
    {0x0A, "G"}, {0x0B, "H"}, {0x0C, "I"}, {0x0D, "J"}, {0x0E, "K"}, {0x0F, "L"},
    {0x10, "M"}, {0x11, "N"}, {0x12, "O"}, {0x13, "P"}, {0x14, "Q"}, {0x15, "R"},
    {0x16, "S"}, {0x17, "T"}, {0x18, "U"}, {0x19, "V"}, {0x1A, "W"}, {0x1B, "X"},
    {0x1C, "Y"}, {0x1D, "Z"}, {0x1E, "1"}, {0x1F, "2"}, {0x20, "3"}, {0x21, "4"},
    {0x22, "5"}, {0x23, "6"}, {0x24, "7"}, {0x25, "8"}, {0x26, "9"}, {0x27, "0"},
    {0x28, "RET"}, {0x29, "ESC"}, {0x2A, "DEL"}, {0x2B, "TAB"}, {0x2C, "SPACE"},
    {0x2D, "-"}, {0x2E, "="}, {0x36, ","}, {0x37, "."}, {0x38, "/"},
};

const char *unify_key_name(uint8_t hid_key)
{
    for (uint8_t i = 0; i < sizeof(c_key_names) / sizeof(c_key_names[0]); i++)
        if (c_key_names[i].code == hid_key)
            return c_key_names[i].name;
    return 0;
}

const char *unify_kind_name(uint8_t kind)
{
    switch (kind)
    {
    case UNIFY_KEEPALIVE:
        return "KEEP-ALIVE";
    case UNIFY_KEY:
        return "KEYSTROKE";
    case UNIFY_MOUSE:
        return "MOUSE";
    case UNIFY_MEDIA:
        return "MEDIA";
    case UNIFY_ENCRYPTED:
        return "ENCRYPTED";
    default:
        return "?";
    }
}

bool unify_decode(const uint8_t *payload, uint8_t len, unify_view_t *out)
{
    if (!payload || !out || len < 5 || len > 22)
        return false;

    // The first byte is the device index, the second the report type
    uint8_t type = payload[1];
    uint8_t want_len;

    switch (type)
    {
    case UNIFY_T_KEEPALIVE:
        want_len = 5;
        break;
    case UNIFY_T_KEY:
    case UNIFY_T_MOUSE:
    case UNIFY_T_MEDIA:
        want_len = 10;
        break;
    case UNIFY_T_ENCRYPTED:
        want_len = 22;
        break;
    default:
        return false; // not a Unifying frame we know
    }

    if (len != want_len)
        return false;
    if (payload[len - 1] != unify_checksum(payload, len))
        return false;

    memset(out, 0, sizeof(*out));
    out->device = payload[0];

    switch (type)
    {
    case UNIFY_T_KEEPALIVE:
        out->kind = UNIFY_KEEPALIVE;
        out->timeout = payload[3];
        return true;

    case UNIFY_T_KEY:
        out->kind = UNIFY_KEY;
        out->modifiers = payload[2];
        memcpy(out->keys, &payload[3], 6);
        return true;

    case UNIFY_T_MOUSE:
    {
        out->kind = UNIFY_MOUSE;
        out->buttons = payload[2];
        // The movement: 24 bits = two 12 bit signed deltas, dx then dy
        uint32_t mov = (uint32_t)payload[4] | ((uint32_t)payload[5] << 8) |
                       ((uint32_t)payload[6] << 16);
        int16_t dx = (int16_t)(mov & 0xFFF);
        int16_t dy = (int16_t)((mov >> 12) & 0xFFF);
        if (dx & 0x800)
            dx |= (int16_t)0xF000;
        if (dy & 0x800)
            dy |= (int16_t)0xF000;
        out->dx = dx;
        out->dy = dy;
        return true;
    }

    case UNIFY_T_MEDIA:
        out->kind = UNIFY_MEDIA;
        out->media = payload[2];
        return true;

    default:
        out->kind = UNIFY_ENCRYPTED;
        return true;
    }
}

uint8_t unify_build_keystroke(uint8_t *out, uint8_t max, uint8_t device, uint8_t modifiers,
                              uint8_t hid_key)
{
    if (!out || max < 10)
        return 0;

    memset(out, 0, 10);
    out[0] = device;
    out[1] = UNIFY_T_KEY;
    out[2] = modifiers;
    out[3] = hid_key;
    out[9] = unify_checksum(out, 10);
    return 10;
}

uint8_t unify_build_release(uint8_t *out, uint8_t max, uint8_t device)
{
    if (!out || max < 10)
        return 0;

    memset(out, 0, 10);
    out[0] = device;
    out[1] = UNIFY_T_KEY;
    out[9] = unify_checksum(out, 10);
    return 10;
}

uint8_t unify_build_keepalive(uint8_t *out, uint8_t max, uint8_t device)
{
    if (!out || max < 5)
        return 0;

    memset(out, 0, 5);
    out[0] = device;
    out[1] = UNIFY_T_KEEPALIVE;
    out[3] = 0x0A; // 10 ms
    out[4] = unify_checksum(out, 5);
    return 5;
}
