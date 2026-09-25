/**
 * The RADAR screen's engine: persistent signal tracking, the drone and
 * video transmitter detector, the bug hunt and the microwave leak test.
 *
 * The sweep engine (scanner.c) is driven one sweep per call and its
 * per-channel results (peak, weak, busy share) accumulate here into a
 * per-channel activity record over a few seconds. Channels that stay busy
 * above the noise floor become "signals": a persistent-carrier table with
 * the classic verdicts -
 *
 *   a wide, gapless, 100% duty signal  -> ANALOG VIDEO (an AV sender, a
 *                                         2.4GHz camera, a baby monitor,
 *                                         an FPV transmitter on 2.4)
 *   one channel continuous + beacons    -> WIFI VIDEO (a WiFi drone or
 *                                         any streaming camera)
 *   many channels, low duty on each     -> HOPPING (DJI-style 2.4 link)
 *   short narrow frames                 -> an ESB style control link
 *
 * The hunt mode keeps a baseline and reports what appears after it. The
 * microwave test parks on 2450 and watches the level and the mains-locked
 * wobble of a leaky oven.
 *
 * Bookkeeping is pure (host tested); the radio parts call scanner_park()
 * and the sweep, both of which hand the radio back.
 */
#ifndef PIXLA_RADAR_H
#define PIXLA_RADAR_H

#include <stdbool.h>
#include <stdint.h>

#include "classify.h"

// Channels the radar watches: the ISM band 2400..2483
#define RADAR_START_MHZ 2400
#define RADAR_END_MHZ 2483
#define RADAR_CHANS (RADAR_END_MHZ - RADAR_START_MHZ + 1)

// A channel counts as active when it stays this much above the floor
#define RADAR_BUSY_MARGIN_DB 8

// Sweeps one window accumulates before the table is rebuilt
#define RADAR_WINDOW_SWEEPS 4

// How many windows a channel must be active in to become a signal
#define RADAR_PERSIST_WINDOWS 2

// Signal table size
#define RADAR_MAX_SIGNALS 6

// What a tracked signal looks like
typedef enum
{
    RADAR_VIDEO = 0,  // wide, gapless: an analogue AV sender or camera
    RADAR_WIFI,       // one busy WiFi channel, streaming shaped
    RADAR_HOPPER,     // seen on many channels: a hopping drone link
    RADAR_CONTROL,    // short narrow frames: an ESB style control link
    RADAR_CARRIER,    // steady narrow carrier: CW, telemetry, a jammer
    RADAR_KIND_COUNT
} radar_kind_t;

const char *radar_kind_name(radar_kind_t kind);

typedef struct
{
    uint16_t mhz;
    uint8_t peak_db;    // strongest level above the floor, in dB
    uint8_t busy_pct;   // median busy share of the active windows
    uint8_t width_mhz;  // contiguous busy channels around the centre
    uint8_t windows;    // windows it was active in (of the recent past)
    uint8_t kind;       // radar_kind_t
    bool first_seen;    // new since the hunt baseline was taken
} radar_signal_t;

typedef struct
{
    // Per channel, over the last RADAR_PERSIST_WINDOWS+1 windows
    uint8_t busy_hist[RADAR_CHANS]; // bit per window, bit 0 newest
    uint8_t busy_pct[RADAR_CHANS];  // median busy share while active
    uint8_t peak_db[RADAR_CHANS];   // strongest dB above floor
    uint8_t floor[RADAR_CHANS];     // the noise floor used

    uint8_t sweeps;      // sweeps into the current window
    uint32_t busy_sum[RADAR_CHANS];  // busy accumulator, this window
    uint32_t peak_sum[RADAR_CHANS];  // strongest peak, this window

    radar_signal_t sig[RADAR_MAX_SIGNALS];
    uint8_t n_sig;

    bool hunting;         // the hunt baseline is active
    uint8_t baseline_pct[RADAR_CHANS]; // busy share at baseline time

    uint16_t hop_chans;   // channels that moved in the last window, for
                          // the hopper verdict
    uint32_t hops;        // hop-like transitions seen (channel changed
                          // between strong packets)
} radar_work_t;

// ---------------------------------------------------------------------------
// Bookkeeping (host testable)
// ---------------------------------------------------------------------------

// Reset the state. Call before the first sweep.
void radar_init(radar_work_t *w);

// One sweep result (the g_scan of scanner.c, caller maps it to the
// RADAR_CHANS range). busy/peak are the scanner's values, floor the
// tracked noise floor per channel. Rebuilds the signal table every
// RADAR_WINDOW_SWEEPS sweeps.
void radar_sweep(radar_work_t *w, const uint8_t *busy, const uint8_t *peak,
                 const uint8_t *floor);

// The signal table, strongest first
uint8_t radar_signals(const radar_work_t *w);
const radar_signal_t *radar_signal(const radar_work_t *w, uint8_t index);

// Rebuilds the table from the accumulated windows. The screen calls this
// from its tick after feeding sweeps; the host tests call it directly.
void radar_signals_rebuild(radar_work_t *w);

// Take the hunt baseline: whatever shows up after this is "new"
void radar_hunt_arm(radar_work_t *w);

// Channels busy right now above the margin: the hopper evidence
uint16_t radar_active_chans(const radar_work_t *w);

// ---------------------------------------------------------------------------
// Verdict helpers
// ---------------------------------------------------------------------------

// Microwave test: level trend of the parked measurement, in dB above the
// floor, and the verdict text for the screen
bool radar_microwave(const burst_t *bursts, uint16_t n, const park_stats_t *stats,
                     uint8_t floor_db, int8_t *out_level, const char **out_verdict);

#endif // PIXLA_RADAR_H
