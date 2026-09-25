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

// Identify listens in windows of this length; the seams between windows are
// blind time the classifier has to tolerate
#define CLASSIFY_WINDOW_MS 250
#define CLASSIFY_WINDOWS 8

// Sweeps taken over the whole span to see whether the emitter hops
#define CLASSIFY_SPREAD_SWEEPS 128

typedef enum
{
    VERDICT_QUIET = 0,
    VERDICT_UNKNOWN,
    VERDICT_CONTINUOUS,   // carrier or jammer: gapless but narrow or unsteady
    VERDICT_WIFI,         // wide and bursty
    VERDICT_WIFI_BEACON,  // wide, sparse, 102.4ms cadence
    VERDICT_MICROWAVE,    // mains locked, wide, very strong
    VERDICT_BLE_ADV,      // narrow, short, on an advertising channel
    VERDICT_NARROW_BURST, // short narrow packets, e.g. a ShockBurst link
    VERDICT_HOPPER,       // present across many channels, low duty each
    VERDICT_BT_CLASSIC,   // 625us slot grid, pseudo-random hops over the whole band
    VERDICT_ANT,          // crystal-exact ~4Hz (ANT+ 8070..8192 counts) on one channel
    VERDICT_VIDEO,        // wide, steady, gapless: an analogue video sender
    VERDICT_RC_FHSS,      // short frames a few ms apart, hopping a small channel set
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

    // Timing of the bursts that count (noise blips dropped, IFS pairs merged)
    uint16_t events;
    uint8_t slot_pct;        // gaps on the 625us Bluetooth slot grid, percent
    uint32_t fixed_period_us; // crystal-tight cadence of 20ms..1s, 0 if none
    uint8_t fixed_pct;       // gaps that fit it (or twice it), percent
    uint32_t frame_us;       // base frame (2..25ms) whose multiples explain the gaps
    uint8_t frame_pct;       // gaps that sit on that frame grid, percent
    uint8_t frame_ratio;     // median gap in frames: 1 = every frame on this channel
    uint8_t frame_multiples; // distinct frame multiples seen, 2+ means the sequence hops
    uint8_t long_gaps;       // silences over 1ms, window seams included
    uint8_t seams;           // window seams in the capture
    uint8_t level_spread;    // dB between strongest and weakest long burst, 255 unknown
    uint16_t mhz;

    // Activity across the swept span, from CLASSIFY_SPREAD_SWEEPS sweeps
    bool spread_measured;
    uint8_t spread_chans;    // channels seen busy at least once
    uint8_t spread_runs;     // separate groups of busy channels
    uint8_t spread_span;     // lowest to highest busy channel, in channels
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

// Feature extraction plus decision; hits (from classify_spread_sample) may be
// NULL when no spread sweeps were taken
void classify_run(uint16_t mhz, const burst_t *bursts, uint16_t count,
                  const park_stats_t *stats, const uint8_t *hits, uint8_t hit_count,
                  verdict_t *out);

// Emission width in MHz around a frequency, taken from the last sweep
uint8_t classify_width(uint16_t mhz);

// Adds the last sweep to a per channel hit count (one byte per swept channel,
// saturating); a hit is a channel well clear of its noise floor
void classify_spread_sample(uint8_t *hits);

// Folds the hit counts of count channels into the spread features
void classify_spread(const uint8_t *hits, uint8_t count, classify_features_t *f);

const char *classify_name(uint8_t kind);

#endif // PIXLA_CLASSIFY_H
