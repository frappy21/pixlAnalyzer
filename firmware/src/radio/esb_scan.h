/**
 * Detection of Nordic ShockBurst / nRF24L01 style traffic - wireless mice,
 * keyboards, presenters and their dongles.
 *
 * These links use no standard access address, so the receiver is armed with a
 * two byte "address" that matches the preamble pattern every ShockBurst packet
 * starts with, and CRC is disabled. What comes back is not a decoded packet:
 * it is evidence that a narrowband burst with ShockBurst timing exists on that
 * frequency. The UI must present it that way.
 */
#ifndef PIXLA_ESB_SCAN_H
#define PIXLA_ESB_SCAN_H

#include <stdbool.h>
#include <stdint.h>

#define ESB_MAX_HITS 12

typedef struct
{
    uint16_t mhz;
    uint16_t packets;  // preamble locks in the dwell window
    uint8_t peak_rssi; // strongest sample, in -dBm
    uint8_t rate;      // 1 = 1Mbit, 2 = 2Mbit
} esb_hit_t;

void esb_scan_init(void);
void esb_scan_reset(void);

// Sweeps the channel list, dwelling on each, and records where ShockBurst like
// activity appeared. Returns the number of channels with hits.
uint8_t esb_scan_run(uint16_t start_mhz, uint16_t end_mhz, uint16_t dwell_ms);

uint8_t esb_scan_count(void);
const esb_hit_t *esb_scan_hit(uint8_t index);
uint32_t esb_scan_total(void);

#endif // PIXLA_ESB_SCAN_H
