/**
 * RC toy link decoders: the 2.4GHz protocols cheap RC helicopters, quads and
 * cars actually run on Enhanced ShockBurst silicon (nRF24L01 clones, XN297).
 *
 * Everything here is pure byte arithmetic over a decoded ESB payload, host
 * tested. The layouts come from the nRF24 Multiprotocol TX module sources
 * (goebish/nrf24_multipro, pascallanger/DIY-Multiprotocol-TX-Module), which
 * are the de facto reference implementations of these links.
 *
 * A "profile" is one protocol: its on-air address (or just the first address
 * byte, A0, that the promiscuous receiver has to be armed for), its payload
 * length, its checksum and how the four sticks sit in the payload.
 *
 * Sticks are normalised: throttle 0..255, yaw/pitch/roll -100..100 (0 is
 * centred), flags as generic bits. The raw payload stays available to the
 * screens: the lab user can always look at the bytes themselves.
 *
 * The generic tracker (rc_track_t) needs no protocol knowledge at all: it
 * watches which payload bytes of a repeating link actually move and turns
 * those into bars - an unknown or bind-scrambled protocol still shows its
 * sticks that way.
 */
#ifndef PIXLA_RC_PROTO_H
#define PIXLA_RC_PROTO_H

#include <stdbool.h>
#include <stdint.h>

// Number of payload bytes a stick tracker watches
#define RC_TRACK_MAX 20

// Generic flags, mapped from each protocol's own bits
#define RC_FLAG_FLIP 0x01
#define RC_FLAG_RTH 0x02
#define RC_FLAG_HEADLESS 0x04
#define RC_FLAG_VIDEO 0x08
#define RC_FLAG_PHOTO 0x10
#define RC_FLAG_RATE 0x20 // dual/triple rate selected
#define RC_FLAG_LED 0x40

// Battery nibbles/levels some telemetried toys carry, 255 = unknown
#define RC_BATT_UNKNOWN 255

typedef struct
{
    uint8_t throttle; // 0..255
    int8_t yaw;       // -100..100
    int8_t pitch;     // -100..100
    int8_t roll;      // -100..100
    uint8_t flags;    // RC_FLAG_*
    uint8_t battery;  // RC_BATT_UNKNOWN or 0..100
} rc_sticks_t;

// Bind packet payload of a protocol that derives its data address from a
// random id exchanged at bind time (Bayang, SymaX). Length 15/10 bytes.
typedef struct
{
    uint8_t data_addr[5];  // the address the link moves to after binding
    uint8_t chan_mhz[4];   // the hopping channel list, absolute MHz
    uint8_t chan_count;    // valid entries, 0 = single channel
    uint8_t txid[3];       // the id the checksums and hop seed use
} rc_bind_t;

// ---------------------------------------------------------------------------
// First-address-byte candidates for the promiscuous receiver
// ---------------------------------------------------------------------------

// The A0 values worth arming the receiver for: the protocol table's own
// first bytes plus the generic nRF24 families (0x55/0xAA/0xE7) and the
// Logitech pairing address byte. The sniffer cycles through these.
uint8_t rc_a0_candidates(const uint8_t **out);

// ---------------------------------------------------------------------------
// Protocol table
// ---------------------------------------------------------------------------

// Index into the static table, -1 = unknown
typedef int8_t rc_proto_t;
#define RC_PROTO_UNKNOWN (-1)

// Disambiguate protocols that share an on-air address using the actual
// payload. rc_proto_by_addr() returns the widest match (e.g. RC_BAYANG for
// any {0x00,0x00,...} capture); call this with the first real payload to get
// a narrower answer. Returns proto unchanged if no refinement is available.
rc_proto_t rc_proto_refine(rc_proto_t proto, const uint8_t *payload, uint8_t len);

// Byte-exact checksum of a payload per its protocol; also used to verify
uint8_t rc_proto_checksum(rc_proto_t proto, const uint8_t *payload, uint8_t len);

// Which protocols exist and what they are called
uint8_t rc_proto_count(void);
const char *rc_proto_name(rc_proto_t proto);

// The first on-air address byte every packet of this protocol carries: the
// value the promiscuous receiver has to be armed for. Profiles whose data
// address is random per bind still have a fixed bind address.
uint8_t rc_proto_a0(rc_proto_t proto);

// Full known address (bind address for bind-derived protocols), 0 when the
// data address is learned at bind. Length always 5.
const uint8_t *rc_proto_addr(rc_proto_t proto);
bool rc_proto_addr_fixed(rc_proto_t proto); // data address known up front

// Bit rate: 1 = 1 Mbit, 2 = 2 Mbit
uint8_t rc_proto_rate(rc_proto_t proto);

// Match a decoded ESB address against the table: the full address for fixed
// profiles, the bind address for bind-derived ones. Length 2..5.
rc_proto_t rc_proto_by_addr(const uint8_t *addr, uint8_t addr_len);

// Expected payload length, 0 = not fixed
uint8_t rc_proto_payload_len(rc_proto_t proto);

// ---------------------------------------------------------------------------
// Payload decoding and building
// ---------------------------------------------------------------------------

// Verifies the checksum and decodes the sticks. False on a bad checksum, a
// wrong length or an unknown protocol.
bool rc_decode(rc_proto_t proto, const uint8_t *payload, uint8_t len, rc_sticks_t *out);

// Writes the sticks back into a payload of a frame that already decodes:
// moves the stick bytes and recomputes the checksum. The bytes that are not
// sticks are kept as captured, so the frame stays credible to the receiver.
// False when the length does not match the protocol.
bool rc_build(rc_proto_t proto, uint8_t *payload, uint8_t len, const rc_sticks_t *sticks);

// Decodes a bind packet (only the bind-derived protocols have one: their
// bind payload is what carries the id the data link uses).
bool rc_bind_decode(rc_proto_t proto, const uint8_t *payload, uint8_t len, rc_bind_t *out);

// Builds a bind packet for self owned receivers: writes the id, the data
// address and the channel list into payload and fixes its checksum.
bool rc_bind_build(rc_proto_t proto, uint8_t *payload, uint8_t len, const rc_bind_t *bind);

// ---------------------------------------------------------------------------
// Generic stick tracker: no protocol knowledge
// ---------------------------------------------------------------------------

typedef struct
{
    uint8_t len;                   // payload length being tracked
    uint8_t min[RC_TRACK_MAX];     // min of each byte
    uint8_t max[RC_TRACK_MAX];     // max of each byte
    uint16_t seen;                 // packets fed
} rc_track_t;

// Reset the tracker for payloads of this length (2..RC_TRACK_MAX)
void rc_track_start(rc_track_t *t, uint8_t len);

// Feed one payload; the byte range over the whole session says which bytes
// are the live ones (sticks) and which are headers
void rc_track_feed(rc_track_t *t, const uint8_t *payload, uint8_t len);

// A byte that moved at least this much counts as a stick
#define RC_TRACK_RANGE 12

// Bitmask (up to RC_TRACK_MAX bits, LSB = byte 0) of the bytes that move
uint32_t rc_track_live(const rc_track_t *t);

// Centre estimate of a byte: the midpoint of its observed range, so a live
// byte can be shown as a bar around a centre even without a protocol
uint8_t rc_track_centre(const rc_track_t *t, uint8_t index);

#endif // PIXLA_RC_PROTO_H
