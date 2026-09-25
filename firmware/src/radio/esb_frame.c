#include <string.h>

#include "esb_frame.h"

// ---------------------------------------------------------------------------
// Bit access: the capture is packed most significant bit first, the same way
// the radio in big endian mode delivers it.
// ---------------------------------------------------------------------------

static inline bool air_bit(const uint8_t *p, uint16_t n)
{
    return (p[n >> 3] >> (7 - (n & 7))) & 1;
}

static inline void air_bit_set(uint8_t *p, uint16_t n, bool v)
{
    uint8_t mask = (uint8_t)(1u << (7 - (n & 7)));
    if (v)
        p[n >> 3] |= mask;
    else
        p[n >> 3] &= (uint8_t)~mask;
}

// Reads 16 on-air bits as a word: MSB first, or LSB first
static uint16_t air_word16(const uint8_t *p, uint16_t start, bool lsb)
{
    uint16_t v = 0;
    for (uint16_t i = 0; i < 16; i++)
    {
        if (air_bit(p, start + i))
            v |= (uint16_t)(1u << (lsb ? i : 15 - i));
    }
    return v;
}

// Extracts n payload bytes starting at on-air bit start
static void air_bytes(const uint8_t *p, uint16_t start, uint8_t *out, uint8_t n, bool lsb)
{
    for (uint8_t j = 0; j < n; j++)
    {
        uint8_t v = 0;
        for (uint8_t k = 0; k < 8; k++)
        {
            if (air_bit(p, start + 8 * j + k))
                v |= (uint8_t)(1u << (lsb ? k : 7 - k));
        }
        out[j] = v;
    }
}

// ---------------------------------------------------------------------------
// CRC
// ---------------------------------------------------------------------------

uint16_t esb_crc16_serial(const uint8_t *air, uint16_t nbits)
{
    uint16_t crc = 0xFFFF;
    for (uint16_t n = 0; n < nbits; n++)
    {
        uint16_t fb = ((crc >> 15) & 1) ^ (air_bit(air, n) ? 1 : 0);
        crc = (uint16_t)(crc << 1);
        if (fb)
            crc ^= 0x1021;
    }
    return crc;
}

uint16_t esb_crc16_shiftin(const uint8_t *air, uint16_t nbits)
{
    uint16_t crc = 0xFFFF;
    for (uint16_t n = 0; n < nbits; n++)
    {
        uint16_t msb = crc & 0x8000;
        crc = (uint16_t)((crc << 1) | (air_bit(air, n) ? 1 : 0));
        if (msb)
            crc ^= 0x1021;
    }
    return crc;
}

// The CRC of one model, or 0xFFFF when the model is out of range
static uint16_t crc_of(uint8_t model, const uint8_t *air, uint16_t nbits)
{
    switch (model)
    {
    case 0: // feedback, MSB bytes
    case 1: // feedback, LSB bytes
        return esb_crc16_serial(air, nbits);
    default: // shift-in
        return esb_crc16_shiftin(air, nbits);
    }
}

// ---------------------------------------------------------------------------
// Decode
// ---------------------------------------------------------------------------

bool esb_frame_decode(const uint8_t *cap, uint8_t cap_len, uint8_t first_addr, bool payload_lsb,
                      esb_frame_t *out)
{
    for (uint8_t addr_len = 5; addr_len >= 2; addr_len--)
    {
        // The capture starts L-1 bytes into the address. Everything from
        // there on is the bit stream: PCF (9) + payload + CRC (16).
        uint8_t off = (uint8_t)(addr_len - 1);
        if (cap_len <= off + 2) // not even PCF and one CRC byte
            continue;

        uint8_t plen = cap[off] >> 2;
        if (plen > ESB_MAX_PAYLOAD)
            continue;

        uint16_t nbits = (uint16_t)(9 + 8 * plen);
        uint8_t span = (uint8_t)((nbits + 16 + 7) / 8);
        if (cap_len < off + span)
            continue;

        const uint8_t *p = cap + off;

        for (uint8_t model = 0; model < ESB_CRC_MODELS; model++)
        {
            uint16_t crc = crc_of(model, p, nbits);
            bool lsb = (model & 1) != 0;
            if (air_word16(p, nbits, lsb) != crc)
                continue;

            if (out)
            {
                memset(out, 0, sizeof(*out));
                out->addr[0] = first_addr;
                for (uint8_t i = 1; i < addr_len; i++)
                    out->addr[i] = cap[i - 1];
                out->addr_len = addr_len;
                out->plen = plen;
                out->pid = cap[off] & 0x03;
                out->noack = (cap[off + 1] & 0x80) != 0;
                air_bytes(p, 9, out->payload, plen, payload_lsb);
                out->payload_lsb = payload_lsb;
                out->crc_model = model;
            }
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// Build
// ---------------------------------------------------------------------------

uint8_t esb_frame_build(uint8_t *out, uint8_t max, const esb_frame_t *f)
{
    if (f->addr_len < 2 || f->addr_len > 5 || f->plen > ESB_MAX_PAYLOAD)
        return 0;

    uint8_t raw_len = esb_frame_raw_len(f);
    if (raw_len == 0 || raw_len > max)
        return 0;

    memset(out, 0, raw_len);

    // Address tail: the bytes after the first one, as captured
    for (uint8_t i = 1; i < f->addr_len; i++)
        out[i - 1] = f->addr[i];

    uint8_t *p = out + (f->addr_len - 1);

    // PCF: 6 bit length, 2 bit PID, then NO_ACK at bit 8
    p[0] = (uint8_t)((f->plen << 2) | (f->pid & 0x03));
    if (f->noack)
        p[1] |= 0x80;

    // Payload, mirroring the bit order the frame carries
    uint16_t nbits = (uint16_t)(9 + 8 * f->plen);
    for (uint8_t j = 0; j < f->plen; j++)
    {
        for (uint8_t k = 0; k < 8; k++)
        {
            bool v = (f->payload[j] & (1u << (f->payload_lsb ? k : 7 - k))) != 0;
            air_bit_set(p, 9 + 8 * j + k, v);
        }
    }

    // CRC over PCF and payload, in the model the frame carries
    uint16_t crc = crc_of(f->crc_model, p, nbits);
    bool lsb = (f->crc_model & 1) != 0;
    for (uint16_t i = 0; i < 16; i++)
    {
        bool v = (crc & (1u << (lsb ? i : 15 - i))) != 0;
        air_bit_set(p, nbits + i, v);
    }

    return raw_len;
}
