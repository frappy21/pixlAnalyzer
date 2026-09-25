/**
 * BLE advertising beacon transmitter: the TX side of the BLE decoder, a lab
 * tool to generate known adverts and check that this device's own sniffer,
 * or any other receiver under test, decodes them.
 *
 * Six payload presets, each built by a pure function (host tested together
 * with the real advert decoder, test/test_beacon.c): a plain name, an Apple
 * iBeacon, Eddystone UID and URL, an AltBeacon, a Microsoft Swift Pair
 * beacon and an Apple type 0x12 offline finding shaped beacon for FindMy
 * detector self tests.
 *
 * The advertising address is a static random address derived from the
 * device's own FICR id, so two units in one lab do not collide.
 *
 * Safety: lowest power default, one advertising channel at a time (37/38/39
 * rotating, like a real beacon), stops itself after BLE_BEACON_MAX_MS, and
 * ble_beacon_stop() from the screen's leave().
 */
#ifndef PIXLA_BLE_BEACON_H
#define PIXLA_BLE_BEACON_H

#include <stdbool.h>
#include <stdint.h>

// Presets, in the order the beacon screen shows them
typedef enum
{
    BLE_BEACON_NAME = 0,
    BLE_BEACON_IBEACON,
    BLE_BEACON_EDDY_UID,
    BLE_BEACON_EDDY_URL,
    BLE_BEACON_ALT,
    BLE_BEACON_SWIFTPAIR,
    BLE_BEACON_FINDMY,
    BLE_BEACON_TYPE_COUNT
} ble_beacon_type_t;

// Longest advertising data a legacy advert carries
#define BLE_BEACON_AD_MAX 31

// Longest unattended run, longer than the TX test's 30s so a scanner has
// time to discover it, still finite
#define BLE_BEACON_MAX_MS 60000u

// Advertising interval choices, indexed by the persisted setting
extern const uint16_t ble_beacon_intervals[5];
#define BLE_BEACON_INTV_COUNT 5

const char *ble_beacon_name(uint8_t type);
const char *ble_beacon_desc(uint8_t type);

// Builds the advertising data of one preset into ad (at most 31 bytes) from
// 8 bytes of device unique id (FICR DEVICEID on target). Returns the byte
// count. Pure, host tested: every preset decodes back through
// ble_ad_summarize() with the expected kind.
uint8_t ble_beacon_build(uint8_t type, const uint8_t dev_id[8], uint8_t ad[BLE_BEACON_AD_MAX]);

// The static random advertising address for 8 bytes of device id: the two
// top bits set, the rest derived from the id. On air byte order (addr[0]
// first).
void ble_beacon_addr(const uint8_t dev_id[8], uint8_t addr[6]);

// ---------------------------------------------------------------------------
// Transmitter, target only
// ---------------------------------------------------------------------------

bool ble_beacon_start(uint8_t type, uint8_t power, uint16_t interval_ms, const uint8_t dev_id[8]);
void ble_beacon_stop(void);
bool ble_beacon_active(void);

// Sends one advertising event (one packet, on the next of the three
// advertising channels) when the interval has elapsed. The screen tick calls
// this. Auto stops after BLE_BEACON_MAX_MS.
void ble_beacon_update(uint32_t now_ms);

uint32_t ble_beacon_sent(void);
uint32_t ble_beacon_remaining_ms(uint32_t now_ms);

#endif // PIXLA_BLE_BEACON_H
