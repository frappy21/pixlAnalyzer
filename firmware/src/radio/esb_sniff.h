/**
 * Enhanced ShockBurst sniffer: receives the packets the RC (Mouse/Kbd) screen
 * only counts preamble locks for, and actually decodes them.
 *
 * The promiscuous front end is the one esb_scan.c uses, extended: the
 * receiver is armed with both polarities of the alternating preamble pattern
 * (0xAA55 and 0x55AA) and captures 40 raw bytes after the match, with no
 * length field and no CRC - the CRC is checked in software by esb_frame.c
 * and only CRC valid packets enter the bookkeeping.
 *
 * What it keeps, in caller provided (arena) storage:
 *  - an address book: every distinct address (per address length and rate)
 *    with its strongest RSSI, packet count and when it was last heard,
 *  - a ring of the last ESB_SNIFF_RING decoded packets, raw capture
 *    included, so they can be replayed verbatim,
 *  - one capture mailbox outside the arena, so the ESB TX screen can replay
 *    a chosen packet after the sniffer screen has been left.
 *
 * Pure bookkeeping, host tested (test/test_esb.c); the radio part is target
 * only and hands the radio back to the sweep engine (scanner_init) when a
 * slice is done.
 */
#ifndef PIXLA_ESB_SNIFF_H
#define PIXLA_ESB_SNIFF_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esb_frame.h"

#define ESB_SNIFF_MAX_DEV 12
#define ESB_SNIFF_RING 12
#define ESB_SNIFF_PHY_AUTO 0 // 2Mbit first, then 1Mbit per channel
#define ESB_SNIFF_PHY_2M 1
#define ESB_SNIFF_PHY_1M 2

// First-address-byte candidates one run can cycle through. Two fly at
// once (BASE0/BASE1, both preamble polarities), so the dwell per pair is
// dwell_ms divided by the number of pairs.
#define ESB_SNIFF_A0_MAX 8

typedef struct
{
    uint8_t addr[5];  // full address, addr[0] first on air
    uint8_t addr_len; // 2..5
    uint8_t rate;     // 1 = 1Mbit, 2 = 2Mbit
    uint16_t mhz;     // last channel heard on
    uint8_t rssi;     // strongest, in -dBm
    uint16_t packets; // CRC valid packets
    uint32_t last_ms;
} esb_dev_t;

// One decoded packet with its raw capture, as the packet browser shows it
// and as the replay mailbox stores it
typedef struct
{
    esb_frame_t f;
    uint8_t raw[ESB_CAPTURE_MAX];
    uint8_t raw_len;
    uint8_t rssi;   // in -dBm
    uint8_t rate;   // 1 = 1Mbit, 2 = 2Mbit, the PHY it was heard on
    uint16_t mhz;   // channel heard on
    uint32_t ms;
} esb_pkt_t;

typedef struct
{
    esb_dev_t dev[ESB_SNIFF_MAX_DEV];
    uint8_t n_dev;
    esb_pkt_t pkt[ESB_SNIFF_RING];
    uint8_t ring_head; // next slot to write
    uint8_t ring_count;
    uint32_t locks;   // preamble locks fed
    uint32_t decoded; // of those, CRC valid
} esb_sniff_work_t;

// ---------------------------------------------------------------------------
// Bookkeeping (host testable)
// ---------------------------------------------------------------------------

// Places the state in `mem` (at least sizeof(esb_sniff_work_t), 4 byte
// aligned) and resets it. False if it does not fit.
bool esb_sniff_init(void *mem, size_t size);
void esb_sniff_reset(void);

// Feeds one promiscuous capture. first_addr is the address byte the preamble
// match consumed (0x55 for an 0xAA55 match, 0xAA for 0x55AA). Bookkeeping
// only: no radio, so the host tests can drive it.
void esb_sniff_feed(const uint8_t *cap, uint8_t cap_len, uint8_t first_addr, uint8_t rate,
                    uint16_t mhz, uint8_t rssi, uint32_t now_ms, bool payload_lsb);

uint8_t esb_sniff_dev_count(void);
const esb_dev_t *esb_sniff_dev(uint8_t index);

// Device indices ordered by packet count, most active first
uint8_t esb_sniff_dev_sorted(uint8_t *idx, uint8_t max);

// Ring access, newest first (index 0 is the newest packet). Count saturates
// at ESB_SNIFF_RING.
uint8_t esb_sniff_pkt_count(void);
const esb_pkt_t *esb_sniff_pkt(uint8_t index);

// The capture mailbox: one packet kept outside the arena for the ESB TX
// screen. False when nothing was captured yet.
bool esb_sniff_capture_get(esb_pkt_t *out);
void esb_sniff_capture_set(const esb_pkt_t *p);
void esb_sniff_capture_clear(void);

uint32_t esb_sniff_locks(void);
uint32_t esb_sniff_decoded(void);

// ---------------------------------------------------------------------------
// Receiver (target only)
// ---------------------------------------------------------------------------

// Listens on start..end MHz for dwell_ms per MHz (split over the enabled
// PHYs), feeding every capture to the bookkeeping. rate_mode is one of
// ESB_SNIFF_PHY_*, payload_lsb the payload byte bit order (ESB_BITS_*).
// Owns the radio for that time.
void esb_sniff_run(uint16_t start_mhz, uint16_t end_mhz, uint16_t dwell_ms, uint8_t rate_mode,
                   bool payload_lsb);

// The same, with the first-address-byte candidates to cycle through: the
// known RC toy families (rc_proto.h) instead of just the nRF24 defaults.
// The capture's first address byte comes from the candidate that matched,
// so decoding works exactly as with the generic run.
void esb_sniff_run_a0(uint16_t start_mhz, uint16_t end_mhz, uint16_t dwell_ms, uint8_t rate_mode,
                      bool payload_lsb, const uint8_t *a0_list, uint8_t a0_count);

// One promiscuous capture with a single pair of A0 candidates, for the RC
// dash that locks one channel: listens dwell_ms there, returns when the
// window is up. a0_a/a0_b are the two first address bytes to accept.
void esb_sniff_listen(uint16_t mhz, uint16_t dwell_ms, uint8_t rate_mode, bool payload_lsb,
                      uint8_t a0_a, uint8_t a0_b);

#endif // PIXLA_ESB_SNIFF_H
