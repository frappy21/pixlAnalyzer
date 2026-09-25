/**
 * Enhanced ShockBurst packet transmitter: the TX side of the ESB sniffer,
 * for replaying a captured packet byte for byte or injecting a crafted one
 * on a self owned link in the lab.
 *
 * The radio is set up exactly like the promiscuous front end (big endian,
 * fixed length, no LENGTH field, no CRC), with the full recovered address as
 * the transmit address and the raw capture bytes as the payload - so a
 * replay is bit exact. A crafted packet goes through esb_frame_build()
 * first and is sent the same way.
 *
 * Safety follows the TX test screen's rules: the power defaults to the
 * lowest setting, it stops itself after ESB_TX_MAX_MS, esb_tx_stop() is
 * idempotent and must be called from the screen's leave().
 */
#ifndef PIXLA_ESB_TX_H
#define PIXLA_ESB_TX_H

#include <stdbool.h>
#include <stdint.h>

#include "esb_frame.h"
#include "tx_test.h" // the shared TX power steps

// Longest unattended run, like the TX test carrier
#define ESB_TX_MAX_MS 30000u

typedef struct
{
    uint8_t addr[5];  // address to transmit with, addr[0] first on air
    uint8_t addr_len; // 2..5
    uint8_t rate;     // 1 = 1Mbit, 2 = 2Mbit
    uint16_t mhz;
    uint8_t power;    // tx_power_t index, TX_POWER_* from tx_test.h
    uint16_t count;   // packets to send, 0 = until stopped or timed out
    uint16_t gap_us;  // gap between the packets of one burst
    uint8_t raw_len;
    uint8_t raw[ESB_CAPTURE_MAX]; // the bytes after the address, see esb_frame.h
} esb_tx_cfg_t;

bool esb_tx_start(const esb_tx_cfg_t *cfg);
void esb_tx_stop(void);
bool esb_tx_active(void);

// Sends the packets of one burst. The screen tick calls this while the
// transmitter is active; one burst is short enough that the buttons stay
// responsive.
void esb_tx_pump(void);

// Auto stop and the countdown for the screen
void esb_tx_update(uint32_t now_ms);
uint32_t esb_tx_remaining_ms(uint32_t now_ms);

uint32_t esb_tx_sent(void);

#endif // PIXLA_ESB_TX_H
