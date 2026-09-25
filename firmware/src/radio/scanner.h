/**
 * RSSI sweep engine.
 *
 * The receiver is tuned to each channel in turn and sampled repeatedly while
 * it stays in RXIDLE, so a visit measures a real slice of time instead of one
 * blind instant. With fast ramp-up a 84 channel sweep with 32 samples per
 * channel costs about 6ms, against 12ms for the original one-sample-per-channel
 * loop that observed the air 0.0017% of the time.
 */
#ifndef PIXLA_SCANNER_H
#define PIXLA_SCANNER_H

#include <stdbool.h>
#include <stdint.h>

#include "app_config.h"

typedef struct
{
    uint8_t peak;  // strongest sample of the visit, in -dBm (smaller = stronger)
    uint8_t weak;  // weakest sample of the visit, the noise estimate
    uint8_t busy;  // 0..255, share of samples above the noise floor
} chan_result_t;

// One burst seen in park mode
typedef struct
{
    uint32_t start_us;
    uint16_t len_us;
    uint8_t peak;
} burst_t;

typedef struct
{
    uint32_t window_us;   // how long we listened
    uint32_t on_us;       // total time above the threshold
    uint16_t bursts;      // number of bursts recorded
    uint16_t dropped;     // bursts that did not fit in the caller's buffer
    uint8_t floor_rssi;   // noise floor used for the threshold
    uint8_t peak_rssi;    // strongest sample seen
} park_stats_t;

extern chan_result_t g_scan[SCAN_MAX_CHANNELS];
extern uint8_t g_floor[SCAN_MAX_CHANNELS]; // tracked noise floor per channel

void scanner_init(void);
void scanner_stop(void);

// Starts the HFXO unless the crystal is already the running HFCLK source.
// TIMER0 keeps HFINT running, so STATE alone reads Running without the
// crystal; every radio entry point calls this before tuning.
void radio_hfxo_start(void);

// Disables the radio and waits for DISABLED, skipping the wait when it is
// already disabled (the event never fires in that case).
void radio_disable(void);

// Sets the swept range in MHz, clamped to what the radio can tune
// (2360..2500 with FREQUENCY.MAP, 2400..2500 otherwise)
void scanner_set_span(uint16_t start_mhz, uint16_t end_mhz);

// Explicit channel list, used by the BLE advertising preset
void scanner_set_channels(const uint16_t *mhz, uint8_t count);

void scanner_set_dwell(uint8_t samples);
void scanner_set_shuffle(bool on);

uint8_t scanner_count(void);
uint16_t scanner_mhz(uint8_t index);
uint16_t scanner_span_start(void);
uint16_t scanner_span_end(void);

// One pass over every configured channel, fills g_scan and updates g_floor
void scanner_sweep(void);

// Stay on one frequency and record every burst above the noise floor.
// Returns the number of bursts written to out.
uint16_t scanner_park(uint16_t mhz, uint32_t window_ms, burst_t *out, uint16_t max_bursts,
                      park_stats_t *stats);

// Single RSSI reading, used by the meter screen
uint8_t scanner_measure(uint16_t mhz, uint8_t samples);

#endif // PIXLA_SCANNER_H
