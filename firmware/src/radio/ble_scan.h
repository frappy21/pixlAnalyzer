/**
 * Real BLE advertising receiver.
 *
 * The nRF52832 is a BLE radio, so on the three advertising channels it does not
 * have to guess from RSSI: it receives the packets, checks the CRC and reads
 * the address and the advertising data. That is the one traffic class this
 * device can identify with certainty rather than probability.
 */
#ifndef PIXLA_BLE_SCAN_H
#define PIXLA_BLE_SCAN_H

#include <stdbool.h>
#include <stdint.h>

#define BLE_MAX_DEVICES 40
#define BLE_NAME_LEN 12

typedef enum
{
    BLE_KIND_PLAIN = 0,
    BLE_KIND_APPLE,
    BLE_KIND_FINDMY,   // Apple offline finding, i.e. AirTag class
    BLE_KIND_TILE,
    BLE_KIND_SMARTTAG, // Samsung
    BLE_KIND_GOOGLE_FMDN,
    BLE_KIND_MICROSOFT,
    BLE_KIND_EDDYSTONE,
    BLE_KIND_IBEACON,
} ble_kind_t;

typedef struct
{
    uint8_t addr[6];
    uint8_t addr_type;  // 1 = random
    uint8_t pdu_type;
    int8_t rssi;        // dBm, strongest seen
    uint16_t packets;
    uint16_t company;   // manufacturer company id, 0 if none
    uint8_t kind;       // ble_kind_t
    uint32_t first_ms;
    uint32_t last_ms;
    char name[BLE_NAME_LEN];
} ble_dev_t;

void ble_scan_init(void);
void ble_scan_reset(void);

// Listens on 2402/2426/2480 for the given time and merges what it hears into
// the device table. Returns the number of valid packets received.
uint16_t ble_scan_run(uint32_t window_ms);

uint8_t ble_scan_count(void);
const ble_dev_t *ble_scan_device(uint8_t index);

// Devices sorted by signal strength, strongest first. Fills idx with indices.
uint8_t ble_scan_sorted(uint8_t *idx, uint8_t max);

// Total packets received since the last reset
uint32_t ble_scan_packets(void);

const char *ble_kind_name(uint8_t kind);

#endif // PIXLA_BLE_SCAN_H
