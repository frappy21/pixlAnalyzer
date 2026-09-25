/**
 * Passive IEEE 802.15.4 (Zigbee, Thread) reception on a radio that has no
 * 802.15.4 mode, by the WazaBee technique.
 *
 * 802.15.4 at 2.4GHz is O-QPSK with half sine pulse shaping at 2 Mchip/s,
 * which is the same waveform as MSK at 2 Mbit/s. Each 4 bit symbol is spread
 * to a fixed 32 chip sequence, so on air it looks like a fixed 32 bit pattern
 * to a 2 Mbit GFSK receiver: the BLE 2M mode, without whitening and CRC. The
 * receiver is armed with the MSK form of the SFD symbols as its address,
 * so it starts a "packet" right in the start of frame delimiter, and every
 * received 32 bit word is mapped back to the symbol whose MSK pattern is
 * nearest in Hamming distance. That is the despreading: chip errors cost
 * distance, not the symbol.
 *
 * Receive only. Frames with the MAC security bit set are counted and shown as
 * secured, their payload is never parsed.
 *
 * Limitation of the radio: one reception is at most 255 bytes. After the rest
 * of the SFD (2 bytes) that is 63 symbols, 2 of them the PHY header, which
 * leaves 30 PSDU bytes. Longer frames are "truncated": the MAC header is still decoded, but
 * the FCS cannot be checked.
 */
#ifndef PIXLA_ZB_RX_H
#define PIXLA_ZB_RX_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define ZB_CH_FIRST 11
#define ZB_CH_LAST 26
#define ZB_CH_COUNT 16
#define ZB_CH_MHZ(ch) (2405u + 5u * ((ch) - ZB_CH_FIRST))

#define ZB_MAX_PSDU 127 // aMaxPHYPacketSize
#define ZB_AIR_LEN 255  // one reception: PCNF1.STATLEN maximum
#define ZB_MAX_PANS 20
#define ZB_PAN_SHORTS 12 // short addresses remembered per PAN
#define ZB_PAN_EXTS 4    // extended addresses remembered per PAN

// Where the receiver synchronises: its 32 bit address is the second half of
// SFD symbol 7 followed by the first half of SFD symbol 0xA, so a reception
// starts 16 bits into symbol 0xA and the PHY header begins at byte
// ZB_AIR_PHR. Not the whole of symbol 7, and not symbol 0 of the preamble:
// the MSK patterns of symbols 0..7 are rotations of one another, so the
// pattern of symbol 7 also appears inside the repeating preamble, off by one
// bit only. The straddling window is at least 7 bits from any preamble window.
#define ZB_AIR_PHR 2

// A symbol further than this from its nearest pattern (of 31 compared bits)
// is noise rather than a damaged symbol. Two different patterns are at least
// 13 apart over those 31 bits (k and k+8 are complements, 31 apart), so up to
// 6 flipped bits per symbol always decode to the right symbol.
#define ZB_SYNC_MAX_DIST 6
// A truncated frame, whose FCS is not in the capture, is trusted only if no
// symbol of it is further than this. Random bits reach 6 or less against the
// best of 16 patterns with a probability under 1%, per symbol.
#define ZB_TRUST_DIST 6

typedef enum
{
    ZB_FT_BEACON = 0,
    ZB_FT_DATA = 1,
    ZB_FT_ACK = 2,
    ZB_FT_CMD = 3,
    // 4..7: reserved, multipurpose, fragment, extended (802.15.4-2015)
} zb_frame_type_t;

typedef enum
{
    ZB_AM_NONE = 0,
    ZB_AM_SHORT = 2,
    ZB_AM_EXT = 3,
} zb_addr_mode_t;

// Decoded MAC header (and the Zigbee beacon fields of a beacon)
typedef struct
{
    uint8_t type;    // zb_frame_type_t
    uint8_t version; // 0 = 2003, 1 = 2006, 2 = 2015
    bool security;
    bool pending;
    bool ack_req;
    bool pan_comp;
    bool has_seq;
    uint8_t seq;

    uint8_t dst_mode; // zb_addr_mode_t
    uint8_t src_mode;
    bool has_dst_pan;
    bool has_src_pan;
    uint16_t dst_pan;
    uint16_t src_pan;
    uint8_t dst_addr[8]; // little endian as on air; a short address in [0..1]
    uint8_t src_addr[8];

    uint8_t hdr_len; // MAC header length in bytes, auxiliary security header and IEs excluded

    // The PAN the frame belongs to: the source PAN (also when compressed
    // away), else the destination PAN. Broadcast PAN 0xFFFF does not count.
    bool has_pan;
    uint16_t pan;

    // Beacon frames without security only
    bool beacon;
    uint16_t superframe;   // superframe specification
    bool assoc_permit;     // superframe bit 15, Zigbee's permit join
    bool pan_coord;        // superframe bit 14
    bool beacon_payload;   // a beacon payload was present
    uint8_t proto_id;      // its first byte: 0 Zigbee, 3 Thread 1.x
    uint8_t stack_profile; // Zigbee: 1 = Zigbee 2006, 2 = Zigbee PRO
    uint8_t proto_ver;     // Zigbee: nwkcProtocolVersion
    bool has_ext_pan;
    uint8_t ext_pan[8]; // Zigbee extended PAN ID, little endian as on air
} zb_mac_t;

// Per channel counters
typedef struct
{
    uint16_t good;  // FCS verified
    uint16_t bad;   // FCS failed
    uint16_t trunc; // too long to verify, header trusted by symbol quality
    uint8_t recent; // frames lately, decayed by zb_rx_decay()
    uint8_t rssi;   // strongest frame, -dBm (0 = none yet)
} zb_chan_t;

enum
{
    ZB_PAN_BEACON = 1 << 0,   // a beacon was seen
    ZB_PAN_PJ = 1 << 1,       // last beacon had association permit (permit join)
    ZB_PAN_ZIGBEE = 1 << 2,   // last beacon carried a Zigbee beacon payload
    ZB_PAN_SECURED = 1 << 3,  // MAC security seen
    ZB_PAN_EXT_PAN = 1 << 4,  // ext_pan is valid
    ZB_PAN_COORD = 1 << 5,    // beacon from the PAN coordinator
    ZB_PAN_MORE = 1 << 6,     // more addresses than the lists hold
    // Thread hint: beacon with proto_id 3 (Thread 1.x), or PAN ID 0xFACE
    ZB_PAN_THREAD = 1 << 7,
};

typedef struct
{
    uint16_t pan;
    uint8_t ch;
    uint8_t rssi; // strongest frame, -dBm
    uint16_t frames;
    uint16_t beacons;
    uint16_t secured;
    uint8_t flags; // ZB_PAN_*
    uint8_t proto_id;
    uint8_t stack_profile;
    uint8_t last_type;
    uint8_t last_seq;
    uint8_t n_short;
    uint8_t n_ext;
    uint8_t ext_pan[8];
    uint16_t shorts[ZB_PAN_SHORTS];
    uint8_t exts[ZB_PAN_EXTS][8];
    uint32_t last_ms;
} zb_pan_t;

typedef struct
{
    uint32_t good;
    uint32_t bad;
    uint32_t trunc;
    uint32_t nosync; // address matches that were not followed by an SFD
    uint32_t type[4]; // good or trusted frames by type: beacon, data, ack, command
    uint32_t secured;
} zb_totals_t;

// Everything the receiver keeps, placed in a caller provided buffer (the
// shared arena) by zb_rx_init()
typedef struct
{
    uint32_t msk[16];            // MSK pattern of each symbol, see zb_msk_table()
    uint8_t air[ZB_AIR_LEN + 1]; // radio buffer
    uint8_t psdu[ZB_MAX_PSDU + 1];
    zb_chan_t chan[ZB_CH_COUNT];
    zb_pan_t pan[ZB_MAX_PANS];
    zb_totals_t totals;
    uint8_t n_pans;
} zb_work_t;

// Outcome of one reception
typedef enum
{
    ZB_RX_NOSYNC = 0, // no SFD / PHY header after the address match
    ZB_RX_GOOD,       // complete frame, FCS correct
    ZB_RX_BAD,        // complete frame, FCS wrong
    ZB_RX_TRUNC,      // longer than the capture, header decoded
} zb_rx_status_t;

typedef struct
{
    uint8_t status;   // zb_rx_status_t
    uint8_t len;      // PHY header frame length
    uint8_t got;      // PSDU bytes in the capture
    uint8_t max_dist; // worst symbol distance of the decoded bytes
    bool parsed;      // mac is valid (and was accounted to a PAN)
    zb_mac_t mac;
    uint16_t rest_us; // frame airtime still to come after the capture
} zb_rx_result_t;

// ---------------------------------------------------------------------------
// Pure decoding, host tested
// ---------------------------------------------------------------------------

// Fills table[16] with the MSK form of the 16 symbols, derived from the chip
// sequences of IEEE 802.15.4 (see zb_rx.c). Bit n = MSK bit n on air.
void zb_msk_table(uint32_t table[16]);

// Nearest symbol to a received 32 bit word (bit 0 first on air). Bit 0 is not
// compared: it depends on the last chip of the previous symbol.
uint8_t zb_symbol_decode(const uint32_t table[16], uint32_t word, uint8_t *dist);

// The receiver address, see ZB_AIR_PHR: 32 bits, bit 0 first on air
uint32_t zb_sync_address(const uint32_t table[16]);

// CRC-16 ITU-T as 802.15.4 uses it for the FCS: polynomial x^16+x^12+x^5+1,
// init 0, bits processed LSB first, no final XOR. Over a frame including its
// FCS the result is 0.
uint16_t zb_crc16(const uint8_t *data, uint8_t len);

// Parses the MAC header of a PSDU of which `got` bytes are present. `full` is
// true when the whole frame (FCS included) is present, which is required to
// parse a beacon's payload. Returns false if the header does not fit or uses
// reserved values.
bool zb_mac_parse(const uint8_t *psdu, uint8_t got, bool full, zb_mac_t *mac);

// ---------------------------------------------------------------------------
// Receiver state
// ---------------------------------------------------------------------------

// Places the state in `mem` (at least sizeof(zb_work_t), 4 byte aligned) and
// resets it. False if it does not fit.
bool zb_rx_init(void *mem, size_t size);
void zb_rx_reset(void);

// Decodes one capture (bytes following the address match, bit 0 of byte 0
// first on air) received on `ch`, and accounts for it
void zb_rx_feed(const uint8_t *air, uint16_t len, uint8_t ch, uint8_t rssi, uint32_t now_ms,
                zb_rx_result_t *res);

// Listens on 802.15.4 channel `ch` for dwell_ms, decoding every frame. Owns the
// radio for that time and hands it back to the RSSI scanner (scanner_init).
// Target only.
void zb_rx_run(uint8_t ch, uint16_t dwell_ms);

// Halves the "recent" activity of every channel; call about once a second
void zb_rx_decay(void);

const zb_totals_t *zb_rx_totals(void);
const zb_chan_t *zb_rx_chan(uint8_t ch);
uint8_t zb_rx_pan_count(void);
const zb_pan_t *zb_rx_pan(uint8_t index);

// PAN indices ordered by frame count, most active first. Returns the count.
uint8_t zb_rx_sorted(uint8_t *idx, uint8_t max);

#endif // PIXLA_ZB_RX_H
