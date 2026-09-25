/**
 * Enhanced ShockBurst (ESB, nRF24 family) frame model: pure functions over
 * the raw bytes a promiscuous capture yields, host tested.
 *
 * An ESB packet on air is: preamble (1 byte, alternating bits), address
 * (2..5 bytes, most significant byte first), a 9 bit packet control field
 * (6 bit payload length, 2 bit PID, 1 bit NO_ACK), 0..32 payload bytes and a
 * 2 byte CRC (x^16+x^12+x^5+1, init 0xFFFF) over the PCF and payload.
 *
 * The promiscuous receiver (esb_sniff.c) matches the alternating preamble
 * pattern as a two byte address, so one capture starts right after the
 * preamble and the first address byte: for an address length L the capture
 * holds L-1 address bytes followed by the PCF, the payload and the CRC, all
 * packed most significant bit first.
 *
 * Bit order: the address and the PCF are documented as most significant bit
 * first. The bit order of the payload bytes and of the CRC bytes is less
 * clearly documented, so the CRC byte order is tried both ways here (the CRC
 * itself decides which one is right) and the payload byte order is left to
 * the caller - it cannot be told apart by the CRC, and the setting exists so
 * a lab can flip it against a live device.
 */
#ifndef PIXLA_ESB_FRAME_H
#define PIXLA_ESB_FRAME_H

#include <stdbool.h>
#include <stdint.h>

#define ESB_MAX_PAYLOAD 32

// Longest capture the sniffer hands over: 4 address tail bytes (L=5) plus
// PCF (9 bits) plus 32 payload bytes plus 2 CRC bytes, rounded to bytes
#define ESB_CAPTURE_MAX 40

// Bit order of the payload bytes on air
#define ESB_BITS_MSB 0
#define ESB_BITS_LSB 1

// The CRC engine model that validated, see the decoder. The nRF24
// documentation calls the CRC "CCITT" (poly x^16+x^12+x^5+1, init 0xFFFF)
// without pinning down the serial form or the byte order, so both serial
// forms and both byte orders are tried and the one that validates is kept.
#define ESB_CRC_FB 0 // feedback form: the classic CRC-16/CCITT-FALSE serial
#define ESB_CRC_SI 1 // shift-in form: the BLE style engine, data through the register
#define ESB_CRC_MODELS 4 // {FB,SI} x {MSB,LSB bytes}

typedef struct
{
    uint8_t addr[5];  // full address, addr[0] first on air, addr_len valid bytes
    uint8_t addr_len; // 2..5
    uint8_t plen;     // payload length from the PCF
    uint8_t pid;      // 2 bit packet id from the PCF
    bool noack;       // NO_ACK bit from the PCF
    uint8_t payload[ESB_MAX_PAYLOAD];
    bool payload_lsb; // payload byte bit order this frame was decoded/built with
    uint8_t crc_model; // which of the ESB_CRC_* models validated (see the header)
} esb_frame_t;

// CRC-16/CCITT serial over the on-air bits: poly x^16+x^12+x^5+1 (0x1021),
// init 0xFFFF, the feedback form. Over the bytes
// 31 32 33 34 35 36 37 38 39 ("123456789", 72 bits) the result is 0x29B1,
// which the host test checks.
uint16_t esb_crc16_serial(const uint8_t *air, uint16_t nbits);

// The shift-in form of the same polynomial: data bits are shifted through
// the register (the engine the nRF5x radio's CRC uses for BLE), which gives
// different results for the same input.
uint16_t esb_crc16_shiftin(const uint8_t *air, uint16_t nbits);

// Decodes one promiscuous capture. `first_addr` is the address byte the
// preamble match consumed: 0x55 when the receiver matched 0xAA 0x55, 0xAA
// when it matched 0x55 0xAA. `payload_lsb` selects the payload byte bit
// order (ESB_BITS_*). Returns false when no address length 2..5 yields a
// frame whose CRC verifies.
bool esb_frame_decode(const uint8_t *cap, uint8_t cap_len, uint8_t first_addr, bool payload_lsb,
                      esb_frame_t *out);

// Serialises a frame for raw transmission: the bytes that follow the on-air
// address, ready to be sent with the receiver's own capture conventions
// (fixed length, no LENGTH field). Mirrors esb_frame_decode. Returns the
// byte count, 0 when it does not fit into max.
uint8_t esb_frame_build(uint8_t *out, uint8_t max, const esb_frame_t *f);

// Bytes a capture of this frame needs, including the address tail: the raw
// length a replay transmits after the configured address
static inline uint8_t esb_frame_raw_len(const esb_frame_t *f)
{
    // (L-1) address tail + PCF(9) + payload + CRC(16), rounded up to bytes
    return (uint8_t)(f->addr_len - 1 + (9 + 8 * f->plen + 16 + 7) / 8);
}

#endif // PIXLA_ESB_FRAME_H
