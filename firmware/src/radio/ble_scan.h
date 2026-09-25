/**
 * Real BLE advertising receiver.
 *
 * The nRF52832 is a BLE radio, so on the three advertising channels it does not
 * have to guess from RSSI: it receives the packets, checks the CRC and reads
 * the address and the advertising data. That is the one traffic class this
 * device can identify with certainty rather than probability.
 *
 * Passive only: it never sends a scan request, let alone anything else.
 *
 * Every valid packet goes to the device table (ble_devtab.h) when one is
 * attached, and to the listener when one is set.
 *
 * Extended advertising: ADV_EXT_IND on the primary channels (1M PHY) is
 * received and its AuxPtr decoded. The AUX_ADV_IND it points to is chased
 * when it is on the 1M or 2M PHY and due within BLE_AUX_MAX_US: the receiver
 * retunes to that data channel, takes the one packet and comes back. Coded
 * PHY (long range) adverts are not received at all, and chained data
 * (AUX_CHAIN_IND) and periodic advertising are not followed.
 */
#ifndef PIXLA_BLE_SCAN_H
#define PIXLA_BLE_SCAN_H

#include <stdbool.h>
#include <stdint.h>

#include "ble_devtab.h"

// Longest wait for an AUX_ADV_IND, measured from its ADV_EXT_IND. The scan
// blocks for that long, so keep it short.
#define BLE_AUX_MAX_US 12000u

typedef void (*ble_scan_listener_t)(const ble_rx_t *rx);

void ble_scan_init(void);

// Clears the packet counters (not the device table, see ble_devtab_clear)
void ble_scan_reset(void);

// Called for every valid packet with AD data; NULL removes it
void ble_scan_set_listener(ble_scan_listener_t listener);

// Listens on 2402/2426/2480 for the given time and hands what it hears to the
// device table and the listener. Returns the number of valid packets received.
// Leaves the radio to the sweep engine (scanner_init) when it returns.
uint16_t ble_scan_run(uint32_t window_ms);

// Total packets received since the last reset
uint32_t ble_scan_packets(void);

// Extended advertising statistics since the last reset
typedef struct
{
    uint32_t ext;       // ADV_EXT_IND received
    uint32_t aux_tried; // AUX_ADV_IND listened for
    uint32_t aux_ok;    // and received
    uint32_t aux_far;   // not followed: due later than BLE_AUX_MAX_US or too soon
    uint32_t aux_coded; // not followed: on the coded PHY
    uint8_t last_chan;  // AuxPtr of the last ADV_EXT_IND
    uint8_t last_phy;
    uint32_t last_offset_us;
} ble_ext_stats_t;

const ble_ext_stats_t *ble_scan_ext_stats(void);

#endif // PIXLA_BLE_SCAN_H
