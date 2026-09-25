#include <string.h>

#include "rc_proto.h"

// ---------------------------------------------------------------------------
// The protocol table. Layouts from the nRF24 Multiprotocol TX module
// (goebish/nrf24_multipro and pascallanger/DIY-Multiprotocol-TX-Module):
//
// BAYANG   BayangToys X6/X7/X9, Eachine E010/H8 mini, JJRC JJ850, Floureon
//          H101. Bind address 00 00 00 00 00 on channel 2400, then the data
//          address and hop list are in the bind packet. Payload 15 bytes,
//          sticks as 10-bit pairs with a dynamic trim in the MSB, checksum
//          the additive sum of bytes 0..13.
//
// SYMAX    Syma X5C-1 and the newer 10 byte protocol family. Bind address
//          AB AC AD AE AF, data address the bind id + A2. Payload 10 bytes,
//          sticks as one byte each, checksum XOR of bytes 0..8 plus 0x55.
//
// X5C      Syma X5C old, 16 byte payload, address fixed 6D 6A 73 73 73,
//          additive checksum. Also the MJX family uses this address with a
//          16 byte payload, so the length tells them apart.
//
// H8       Eachine H8 mini 3D, JJRC H20/H22, CG023: fixed address
//          C4 57 09 65 21, payload 20 bytes, sticks at bytes 9..12,
//          additive checksum over bytes 9..18.
//
// MJX      WLtoys Q288-ish, MJX X600/X800/H26D, Eachine E010 (250k):
//          6D 6A 73 73 73 / 6D 6A 77 77 77, payload 16, additive checksum,
//          sticks one byte each sign+magnitude.
//
// Nothing here transmits anything: the same table is the emulator's encoder.
// ---------------------------------------------------------------------------

typedef enum
{
    RC_BAYANG = 0,
    RC_SYMAX,
    RC_X5C,
    RC_H8,
    RC_MJX,
    RC_PROTO_COUNT_
} rc_proto_id_t;

typedef struct
{
    const char *name;
    uint8_t addr[5];  // bind address for bind-derived, data address otherwise
    uint8_t a0;       // first on-air address byte (= addr[0])
    bool bind_derived; // data address learned from the bind packet
    uint8_t plen;
    uint8_t rate;     // 1 = 1Mbit, 2 = 2Mbit
} rc_table_t;

static const rc_table_t c_table[RC_PROTO_COUNT_] = {
    [RC_BAYANG] = {"BAYANG", {0x00, 0x00, 0x00, 0x00, 0x00}, 0x00, true, 15, 1},
    [RC_SYMAX] = {"SYMAX", {0xAB, 0xAC, 0xAD, 0xAE, 0xAF}, 0xAB, true, 10, 1},
    [RC_X5C] = {"X5C", {0x6D, 0x6A, 0x73, 0x73, 0x73}, 0x6D, false, 16, 1},
    [RC_H8] = {"H8", {0xC4, 0x57, 0x09, 0x65, 0x21}, 0xC4, false, 20, 1},
    [RC_MJX] = {"MJX", {0x6D, 0x6A, 0x77, 0x77, 0x77}, 0x6D, false, 16, 1},
};

// The interesting first address bytes the RC receiver should be armed for:
// the table's own A0s plus the generic ones (0x55/0xAA: every nRF24 default
// family; 0xE7: the nRF24 datasheet address; 0xBB: Logitech pairing)
static const uint8_t c_a0_scan[] = {0x55, 0xAA, 0x00, 0xAB, 0x6D, 0xC4, 0xE7, 0xBB};
#define RC_A0_SCAN_COUNT (uint8_t)(sizeof(c_a0_scan) / sizeof(c_a0_scan[0]))

// Exposed to the sniffer: the A0 values worth cycling through
uint8_t rc_a0_candidates(const uint8_t **out)
{
    *out = c_a0_scan;
    return RC_A0_SCAN_COUNT;
}

uint8_t rc_proto_count(void) { return RC_PROTO_COUNT_; }

const char *rc_proto_name(rc_proto_t proto)
{
    if (proto < 0 || proto >= RC_PROTO_COUNT_)
        return "?";
    return c_table[proto].name;
}

uint8_t rc_proto_a0(rc_proto_t proto)
{
    if (proto < 0 || proto >= RC_PROTO_COUNT_)
        return 0x55;
    return c_table[proto].a0;
}

const uint8_t *rc_proto_addr(rc_proto_t proto)
{
    if (proto < 0 || proto >= RC_PROTO_COUNT_)
        return 0;
    return c_table[proto].addr;
}

bool rc_proto_addr_fixed(rc_proto_t proto)
{
    if (proto < 0 || proto >= RC_PROTO_COUNT_)
        return false;
    return !c_table[proto].bind_derived;
}

uint8_t rc_proto_rate(rc_proto_t proto)
{
    if (proto < 0 || proto >= RC_PROTO_COUNT_)
        return 1;
    return c_table[proto].rate;
}

uint8_t rc_proto_payload_len(rc_proto_t proto)
{
    if (proto < 0 || proto >= RC_PROTO_COUNT_)
        return 0;
    return c_table[proto].plen;
}

rc_proto_t rc_proto_by_addr(const uint8_t *addr, uint8_t addr_len)
{
    if (!addr || addr_len < 2)
        return RC_PROTO_UNKNOWN;

    // Fixed address protocols match on the recovered prefix: the capture
    // may only have validated a shorter address length than the true one,
    // and the prefix still identifies the family.
    for (uint8_t p = 0; p < RC_PROTO_COUNT_; p++)
    {
        // The two 6D 6A families need the third byte to tell apart
        if (p == RC_X5C || p == RC_MJX)
        {
            if (addr_len < 3)
                continue;
            if (addr[0] == 0x6D && addr[1] == 0x6A && addr[2] == c_table[p].addr[2])
                return p;
            continue;
        }

        bool match = true;
        for (uint8_t i = 0; i < addr_len && i < 5; i++)
        {
            if (addr[i] != c_table[p].addr[i])
            {
                match = false;
                break;
            }
        }
        if (match)
            return p;
    }
    return RC_PROTO_UNKNOWN;
}

// ---------------------------------------------------------------------------
// Checksums
// ---------------------------------------------------------------------------

uint8_t rc_proto_checksum(rc_proto_t proto, const uint8_t *payload, uint8_t len)
{
    if (proto < 0 || proto >= RC_PROTO_COUNT_ || !payload || !len)
        return 0;

    uint8_t sum = 0;
    switch (proto)
    {
    case RC_BAYANG: // additive over 0..13
        for (uint8_t i = 0; i < 14 && i < len; i++)
            sum += payload[i];
        return sum;

    case RC_SYMAX: // XOR over 0..8, plus 0x55
        sum = 0;
        for (uint8_t i = 0; i < 9 && i < len; i++)
            sum ^= payload[i];
        return (uint8_t)(sum + 0x55);

    case RC_X5C: // additive over 0..14
    case RC_MJX:
        for (uint8_t i = 0; i < 15 && i < len; i++)
            sum += payload[i];
        return sum;

    case RC_H8: // additive over 9..18
        for (uint8_t i = 9; i < 19 && i < len; i++)
            sum += payload[i];
        return sum;

    default:
        return 0;
    }
}

static bool checksum_ok(rc_proto_t proto, const uint8_t *p, uint8_t len)
{
    if (len != c_table[proto].plen)
        return false;
    return p[len - 1] == rc_proto_checksum(proto, p, len);
}

// ---------------------------------------------------------------------------
// Stick scaling helpers
// ---------------------------------------------------------------------------

// A symmetric byte stick centred at 0x80, low half 0x81..0xFF, high half
// 0x00..0x7F (Syma X elevator/rudder style), to -100..100
static int8_t sym_low_first(uint8_t v)
{
    if (v >= 0x80)
        return (int8_t)-((v - 0x80) * 100 / 0x7F);
    return (int8_t)((0x7F - v) * 100 / 0x7F);
}

// The same layout the other way round: high half 0x80..0xFF
static int8_t sym_high_first(uint8_t v)
{
    if (v >= 0x80)
        return (int8_t)((v - 0x80) * 100 / 0x7F);
    return (int8_t)-((0x7F - v) * 100 / 0x7F);
}

// Syma X: elevator and aileron are inverted (low half is stick up), rudder
// is normal. H8 mini: rudder like Syma elevator, elevator around 0x7F with
// a wider range, aileron inverted.
static int8_t h8_scale(uint8_t v, bool inverted)
{
    int16_t s = (int16_t)v - 0x7F; // -127..128
    if (inverted)
        s = -s;
    if (s > 100)
        s = 100;
    if (s < -100)
        s = -100;
    return (int8_t)s;
}

// A 10-bit stick value with the dynamic trim in the MSB byte (Bayang):
// msb holds the top 2 bits in bits 0..1 and the trim in bits 2..7
static uint16_t bayang_10bit(uint8_t msb, uint8_t lsb)
{
    return (uint16_t)(((msb & 0x03) << 8) | lsb);
}

// Sign+magnitude stick centred at 127/128 (MJX): the byte carries
// v*127/100, the decode scales it back
static int8_t mjx_scale(uint8_t v)
{
    int16_t s;
    if (v < 0x80)
        s = (int16_t)((0x7F - v) * 100 / 127);
    else
        s = (int16_t)-((v - 0x80) * 100 / 127);
    if (s > 100)
        s = 100;
    if (s < -100)
        s = -100;
    return (int8_t)s;
}

static int8_t centred_10bit(int32_t v)
{
    // 0..1023, centre 512, linear to -100..100
    int32_t s = (v - 512) * 100 / 512;
    if (s < -100)
        s = -100;
    if (s > 100)
        s = 100;
    return (int8_t)s;
}

// ---------------------------------------------------------------------------
// Decode
// ---------------------------------------------------------------------------

bool rc_decode(rc_proto_t proto, const uint8_t *payload, uint8_t len, rc_sticks_t *out)
{
    if (proto < 0 || proto >= RC_PROTO_COUNT_ || !payload || !out)
        return false;
    if (len != c_table[proto].plen)
        return false;
    if (!checksum_ok(proto, payload, len))
        return false;

    memset(out, 0, sizeof(*out));
    out->battery = RC_BATT_UNKNOWN;

    switch (proto)
    {
    case RC_BAYANG:
    {
        // [0]=0xA5 [1]=mode [2]/[3]=flags [4..11]=AIL ELE THR RUD 10-bit
        // [12]=id [13]=0x0A [14]=checksum
        uint16_t ail = bayang_10bit(payload[5], payload[4]);
        uint16_t ele = bayang_10bit(payload[7], payload[6]);
        uint16_t thr = bayang_10bit(payload[9], payload[8]);
        uint16_t rud = bayang_10bit(payload[11], payload[10]);
        out->roll = centred_10bit(ail);
        out->pitch = centred_10bit(ele);
        out->throttle = (uint8_t)((thr * 255u) / 1023u);
        out->yaw = centred_10bit(rud);
        if (payload[2] & 0x01)
            out->flags |= RC_FLAG_RTH;
        if (payload[2] & 0x02)
            out->flags |= RC_FLAG_HEADLESS;
        if (payload[2] & 0x08)
            out->flags |= RC_FLAG_FLIP;
        if (payload[2] & 0x10)
            out->flags |= RC_FLAG_VIDEO;
        if (payload[2] & 0x20)
            out->flags |= RC_FLAG_PHOTO;
        if (payload[1] != 0xFA)
            out->flags |= RC_FLAG_RATE;
        return true;
    }

    case RC_SYMAX:
    {
        // [0]=THR [1]=ELE [2]=RUD [3]=AIL [4]=flags [5..7]=high bits
        // [8]=0 [9]=checksum
        out->throttle = payload[0];
        out->pitch = sym_low_first(payload[1]);
        out->yaw = sym_high_first(payload[2]);
        out->roll = sym_low_first(payload[3]);
        if (payload[6] & 0x40)
            out->flags |= RC_FLAG_FLIP;
        if (payload[7] & 0x80)
            out->flags |= RC_FLAG_HEADLESS;
        if (payload[4] & 0x40)
            out->flags |= RC_FLAG_VIDEO;
        if (payload[4] & 0x80)
            out->flags |= RC_FLAG_PHOTO;
        if (payload[5] & 0x80)
            out->flags |= RC_FLAG_RATE;
        return true;
    }

    case RC_X5C:
    {
        // Old Syma X5C 16 byte frame: sticks first, additive checksum
        out->throttle = payload[0];
        out->pitch = sym_low_first(payload[1]);
        out->yaw = sym_high_first(payload[2]);
        out->roll = sym_low_first(payload[3]);
        if (payload[6] & 0x40)
            out->flags |= RC_FLAG_FLIP;
        if (payload[7] & 0x80)
            out->flags |= RC_FLAG_HEADLESS;
        return true;
    }

    case RC_H8:
    {
        // [9]=THR [10]=RUD [11]=ELE [12]=AIL [17]/[18]=flags
        out->throttle = payload[9];
        out->yaw = h8_scale(payload[10], true);
        out->pitch = h8_scale(payload[11], false);
        out->roll = h8_scale(payload[12], true);
        if (payload[17] & 0x01)
            out->flags |= RC_FLAG_FLIP;
        if (payload[17] & 0x02)
            out->flags |= RC_FLAG_RATE;
        if (payload[17] & 0x10)
            out->flags |= RC_FLAG_HEADLESS;
        if (payload[17] & 0x20)
            out->flags |= RC_FLAG_RTH;
        if (payload[17] & 0x08)
            out->flags |= RC_FLAG_LED;
        return true;
    }

    case RC_MJX:
    {
        // [0]=THR [1]=RUD [2]=ELE [3]=AIL [10]=flags [14]=more flags
        out->throttle = payload[0];
        out->yaw = mjx_scale(payload[1]);
        out->pitch = mjx_scale(payload[2]);
        out->roll = mjx_scale(payload[3]);
        if (payload[14] & 0x01)
            out->flags |= RC_FLAG_FLIP;
        if (payload[10] & 0x01)
            out->flags |= RC_FLAG_HEADLESS;
        if (payload[10] & 0x02)
            out->flags |= RC_FLAG_RTH;
        if (payload[14] & 0x08)
            out->flags |= RC_FLAG_PHOTO;
        if (payload[14] & 0x10)
            out->flags |= RC_FLAG_VIDEO;
        if (payload[14] & 0x20)
            out->flags |= RC_FLAG_LED;
        return true;
    }

    default:
        return false;
    }
}

// ---------------------------------------------------------------------------
// Build (the emulator side)
// ---------------------------------------------------------------------------

// Inverse of the scalers, clipped
static uint8_t stick_low_first(int8_t v)
{
    // Syma X style: 0x80/0x7F centred, 0xFF = -100, 0x00 = +100
    int16_t half = (int16_t)v * 127 / 100; // -127..127
    return (uint8_t)(0x80 - half);
}

static uint8_t stick_high_first(int8_t v)
{
    // The other polarity: 0xFF = +100, 0x00 = -100
    int16_t half = (int16_t)v * 127 / 100;
    uint8_t b = (uint8_t)(0x80 + half);
    return b == 0x80 ? 0x7F : b;
}

static void bayang_set_10bit(uint8_t *msb, uint8_t *lsb, uint16_t v)
{
    *lsb = (uint8_t)(v & 0xFF);
    *msb = (uint8_t)((*msb & 0xFC) | ((v >> 8) & 0x03));
}

bool rc_build(rc_proto_t proto, uint8_t *payload, uint8_t len, const rc_sticks_t *sticks)
{
    if (proto < 0 || proto >= RC_PROTO_COUNT_ || !payload || !sticks)
        return false;
    if (len != c_table[proto].plen)
        return false;

    switch (proto)
    {
    case RC_BAYANG:
    {
        uint16_t thr = (uint16_t)((uint32_t)sticks->throttle * 1023u / 255u);
        uint16_t ail = (uint16_t)((sticks->roll + 100) * 1023u / 200u);
        uint16_t ele = (uint16_t)((sticks->pitch + 100) * 1023u / 200u);
        uint16_t rud = (uint16_t)((sticks->yaw + 100) * 1023u / 200u);
        bayang_set_10bit(&payload[5], &payload[4], ail);
        bayang_set_10bit(&payload[7], &payload[6], ele);
        bayang_set_10bit(&payload[9], &payload[8], thr);
        bayang_set_10bit(&payload[11], &payload[10], rud);
        break;
    }

    case RC_SYMAX:
    case RC_X5C:
        payload[0] = sticks->throttle;
        payload[1] = stick_low_first(sticks->pitch);
        payload[2] = stick_high_first(sticks->yaw);
        payload[3] = stick_low_first(sticks->roll);
        break;

    case RC_H8:
    {
        // Symmetric with h8_scale(): -100..100 maps 0x7F-100..0x7F+100
        payload[9] = sticks->throttle;
        payload[10] = (uint8_t)(0x7F - sticks->yaw);
        payload[11] = (uint8_t)(0x7F + sticks->pitch);
        payload[12] = (uint8_t)(0x7F - sticks->roll);
        break;
    }

    case RC_MJX:
    {
        payload[0] = sticks->throttle;
        int16_t s;
        s = (int16_t)sticks->yaw * 127 / 100;
        payload[1] = (uint8_t)(s < 0 ? (0x80 + (-s)) : (0x7F - s));
        s = (int16_t)sticks->pitch * 127 / 100;
        payload[2] = (uint8_t)(s < 0 ? (0x80 + (-s)) : (0x7F - s));
        s = (int16_t)sticks->roll * 127 / 100;
        payload[3] = (uint8_t)(s < 0 ? (0x80 + (-s)) : (0x7F - s));
        break;
    }

    default:
        return false;
    }

    payload[len - 1] = rc_proto_checksum(proto, payload, len);
    return true;
}

// ---------------------------------------------------------------------------
// Bind packets
// ---------------------------------------------------------------------------

bool rc_bind_decode(rc_proto_t proto, const uint8_t *payload, uint8_t len, rc_bind_t *out)
{
    if (proto < 0 || !payload || !out)
        return false;

    memset(out, 0, sizeof(*out));

    if (proto == RC_BAYANG && len >= 14)
    {
        // [0]=0xA4 [1..5]=data address [6..9]=channel offsets from 2400
        // [10..12]=id. Some offsets reach past 2500 MHz: this receiver
        // cannot follow those hops, the dash resweeps instead.
        if (payload[0] != 0xA4 && payload[0] != 0xA1 && payload[0] != 0xA2 &&
            payload[0] != 0xA3)
            return false;
        memcpy(out->data_addr, &payload[1], 5);
        for (uint8_t i = 0; i < 4; i++)
            out->chan_mhz[i] = payload[6 + i];
        out->chan_count = 4;
        memcpy(out->txid, &payload[10], 3);
        return true;
    }

    if (proto == RC_SYMAX && len >= 10)
    {
        // [0..4]=address reversed [5..7]=AA AA AA [8]=0 [9]=checksum.
        // The data address keeps bind bytes 1..4 and moves the last byte
        // to the id slot with 0xA2 appended (Multipro: txid[0..3] + 0xA2,
        // on air the id first). The bind capture reversed the address, so
        // payload[0] is bind byte 4.
        const uint8_t *a = c_table[RC_SYMAX].addr;
        for (uint8_t i = 0; i < 5; i++)
            if (payload[i] != a[4 - i])
                return false;
        // on-air data address: [id][bind1][bind2][bind3][0xA2]
        out->data_addr[0] = a[4]; // becomes the id, carried over from bind
        out->data_addr[1] = a[0];
        out->data_addr[2] = a[1];
        out->data_addr[3] = a[2];
        out->data_addr[4] = 0xA2;
        out->chan_count = 0; // single channel, follow by RSSI
        return true;
    }

    return false;
}

bool rc_bind_build(rc_proto_t proto, uint8_t *payload, uint8_t len, const rc_bind_t *bind)
{
    if (proto < 0 || !payload || !bind)
        return false;

    if (proto == RC_BAYANG && len >= 15)
    {
        memset(payload, 0, len);
        payload[0] = 0xA4;
        memcpy(&payload[1], bind->data_addr, 5);
        for (uint8_t i = 0; i < 4; i++)
            payload[6 + i] = bind->chan_mhz[i];
        memcpy(&payload[10], bind->txid, 3);
        payload[13] = 0x0A;
        payload[14] = rc_proto_checksum(proto, payload, len);
        return true;
    }

    return false;
}

// ---------------------------------------------------------------------------
// Generic tracker
// ---------------------------------------------------------------------------

void rc_track_start(rc_track_t *t, uint8_t len)
{
    if (!t)
        return;
    memset(t, 0, sizeof(*t));
    t->len = len > RC_TRACK_MAX ? RC_TRACK_MAX : len;
    memset(t->min, 0xFF, sizeof(t->min));
}

void rc_track_feed(rc_track_t *t, const uint8_t *payload, uint8_t len)
{
    if (!t || !payload || t->len == 0)
        return; // not started
    if (len > t->len)
        len = t->len;

    for (uint8_t i = 0; i < len; i++)
    {
        if (payload[i] < t->min[i])
            t->min[i] = payload[i];
        if (payload[i] > t->max[i])
            t->max[i] = payload[i];
    }
    t->seen++;
}

uint32_t rc_track_live(const rc_track_t *t)
{
    if (!t || t->seen < 3)
        return 0;

    uint32_t mask = 0;
    for (uint8_t i = 0; i < t->len; i++)
    {
        if ((uint8_t)(t->max[i] - t->min[i]) >= RC_TRACK_RANGE)
            mask |= (1u << i);
    }
    return mask;
}

uint8_t rc_track_centre(const rc_track_t *t, uint8_t index)
{
    if (!t || index >= t->len)
        return 0x80;
    return (uint8_t)((t->min[index] + t->max[index]) / 2);
}
