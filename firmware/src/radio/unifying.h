/**
 * Logitech Unifying payload decoder and builder, for lab work on self owned
 * keyboards and mice. Frame layouts from the MouseJack research (Marc
 * Newlin, Bastille, DEF CON 24) and the LOGITacker implementation, byte
 * verified there:
 *
 *   keep-alive  5 bytes  [dev][0x40][0x00][timeout][chk]
 *   keystroke   10 bytes [dev][0xC1][mod][6 HID keys][chk]
 *   mouse move  10 bytes [dev][0xC2][buttons][0][dx dy 12+12 bit][wy][wx][chk]
 *   multimedia  10 bytes [dev][0xC3][4 consumer codes][3 zero][chk]
 *   encrypted   22 bytes [dev][0xD3][7 AES][1][4 counter][7 zero][chk]
 *
 * The checksum is the two's complement of the byte sum: 0 - sum(payload[0
 * .. len-2]), the last byte.
 *
 * Receive only in the sniffer (the detail card names the keys), plus the
 * builders the ESB TX screen uses for injection into one's own receiver.
 */
#ifndef PIXLA_UNIFYING_H
#define PIXLA_UNIFYING_H

#include <stdbool.h>
#include <stdint.h>

typedef enum
{
    UNIFY_NONE = 0,
    UNIFY_KEEPALIVE,
    UNIFY_KEY,
    UNIFY_MOUSE,
    UNIFY_MEDIA,
    UNIFY_ENCRYPTED,
    UNIFY_KIND_COUNT
} unify_kind_t;

typedef struct
{
    uint8_t kind;      // unify_kind_t
    uint8_t device;    // the device index byte (07, 08, ... 00 for the dongle)
    uint8_t modifiers; // HID modifier bitmap for keystrokes
    uint8_t keys[6];   // HID usage codes, 0 padded
    uint8_t buttons;   // mouse button mask
    int16_t dx, dy;    // mouse deltas
    uint8_t media;     // the first consumer page code
    uint8_t timeout;   // keep-alive timeout
} unify_view_t;

// Verifies the checksum and decodes. False when the payload is not a
// Unifying frame or the checksum does not match.
bool unify_decode(const uint8_t *payload, uint8_t len, unify_view_t *out);

// The checksum over the first len-1 bytes, as the last byte carries it
uint8_t unify_checksum(const uint8_t *payload, uint8_t len);

// Builds a keystroke frame (key down) for one HID usage code, with the
// modifiers bitmap. Returns the length, 0 when out is too small (needs 10).
uint8_t unify_build_keystroke(uint8_t *out, uint8_t max, uint8_t device, uint8_t modifiers,
                              uint8_t hid_key);

// The matching release frame (all keys up)
uint8_t unify_build_release(uint8_t *out, uint8_t max, uint8_t device);

// A keep-alive with a 100 ms timeout
uint8_t unify_build_keepalive(uint8_t *out, uint8_t max, uint8_t device);

// A short name for a HID usage code, or 0: "A", "1", "RET", "ESC", ...
const char *unify_key_name(uint8_t hid_key);

// The name of the frame kind
const char *unify_kind_name(uint8_t kind);

#endif // PIXLA_UNIFYING_H
