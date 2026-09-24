/**
 * Traffic classification from RSSI evidence.
 *
 * This device cannot demodulate WiFi, Zigbee or classic Bluetooth, so it never
 * claims to have decoded them. What it can do is measure emission width, duty
 * cycle, burst length and repetition period, and say which technology those
 * numbers are consistent with - together with the numbers themselves, so the
 * user can disagree with it.
 *
 * BLE is the exception: on 2402/2426/2480 the packets can actually be received
 * and CRC checked, and the UI upgrades the verdict to certain when that works.
 */
#ifndef PIXLA_CLASSIFY_H
#define PIXLA_CLASSIFY_H

#include <stdbool.h>
#include <stdint.h>

#include "scanner.h"

typedef enum
{
    VERDICT_QUIET = 0,
    VERDICT_UNKNOWN,
    VERDICT_CONTINUOUS,   // carrier, analogue video, jammer
    VERDICT_WIFI,         // wide and bursty
    VERDICT_WIFI_BEACON,  // wide, sparse, 102.4ms cadence
    VERDICT_MICROWAVE,    // mains locked, wide, very strong
    VERDICT_BLE_ADV,      // narrow, short, on an advertising channel
    VERDICT_NARROW_BURST, // short narrow packets, e.g. a ShockBurst link
    VERDICT_HOPPER,       // present across many channels, low duty each
    VERDICT_COUNT
} verdict_kind_t;

typedef struct
{
    uint32_t duty_ppm;      // share of the window that was occupied
    uint32_t period_us;     // dominant repetition period, 0 if none found
    uint16_t period_spread; // percent jitter of that period
    uint16_t median_len_us;
    uint16_t bursts;
    uint8_t width_mhz;      // contiguous MHz around the frequency that are busy
    uint8_t peak_rssi;      // in -dBm
    bool on_ble_channel;
    bool mains_locked;      // period close to 16.6ms or 20ms
} classify_features_t;

typedef struct
{
    uint8_t kind;       // verdict_kind_t
    uint8_t confidence; // 0..100
    classify_features_t f;
} verdict_t;

// Pure decision step, unit tested on the host
uint8_t classify_decide(const classify_features_t *f, uint8_t *confidence);

// Extracts features from a park capture
void classify_features(uint16_t mhz, const burst_t *bursts, uint16_t count,
                       const park_stats_t *stats, classify_features_t *out);

// Feature extraction plus decision
void classify_run(uint16_t mhz, const burst_t *bursts, uint16_t count,
                  const park_stats_t *stats, verdict_t *out);

// Emission width in MHz around a frequency, taken from the last sweep
uint8_t classify_width(uint16_t mhz);

const char *classify_name(uint8_t kind);

#endif // PIXLA_CLASSIFY_H
