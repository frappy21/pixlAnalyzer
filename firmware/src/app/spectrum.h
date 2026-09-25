/**
 * The display model: live trace, peak hold, max hold, average, min hold, a
 * reference trace, the long window occupancy and the waterfall history.
 *
 * History lives in RAM (the device has 58KB spare) and covers the whole
 * session, so the waterfall can be scrolled back to look at something that
 * already scrolled off the screen. Nothing is written to flash.
 *
 * The trace options (trace mode, RBW, calibration offset) are session only:
 * they start from their defaults after every boot.
 */
#ifndef PIXLA_SPECTRUM_H
#define PIXLA_SPECTRUM_H

#include <stdbool.h>
#include <stdint.h>

#include "app_config.h"

// Rows of waterfall history kept in RAM, 32 bytes each
#define HISTORY_ROWS 384

// What the spectrum bars show. Max hold is drawn as dotted caps in every mode.
typedef enum
{
    TRACE_PEAK = 0, // peak hold decaying at PEAK_DECAY_DB_PER_S
    TRACE_AVG,      // exponential average of the live trace
    TRACE_MIN,      // lowest live value since the last clear: the steady carriers
    TRACE_COUNT
} trace_mode_t;

// Calibration offset range, dB added to every dBm readout
#define SPECTRUM_CAL_MIN_DB -10
#define SPECTRUM_CAL_MAX_DB 10

void spectrum_reset(void);

// Folds the latest sweep into the traces and, every wf_decim sweeps, pushes a
// new waterfall row. Returns true when a new row was pushed.
bool spectrum_update(uint32_t now_ms);

// The split screen parts at their default place (app_config.h layout)
void spectrum_draw(int marker_col);
void spectrum_draw_waterfall(uint16_t scroll_back);
void spectrum_draw_ruler(uint8_t plan, int marker_col);

// Same, anywhere on the screen: the plot over h rows from top (h <= 57), the
// waterfall over rows from top, the ruler with its line at y. delta_col is the
// reference marker, -1 for none.
void spectrum_draw_plot(int top, int h, int marker_col, int delta_col);
void spectrum_draw_waterfall_rows(int top, int rows, uint16_t scroll_back);
void spectrum_draw_ruler_at(int y, uint8_t plan, int marker_col);

// dBm figures on the grid lines of a plot drawn with spectrum_draw_plot(),
// inverted over whatever is under them
void spectrum_draw_db_labels(int top, int h);

void spectrum_snapshot_ref(void);
void spectrum_clear_ref(void);
bool spectrum_has_ref(void);

// Restarts max hold and min hold
void spectrum_clear_max(void);

// Trace mode (trace_mode_t). Selecting min hold restarts it.
void spectrum_set_trace(uint8_t mode);
uint8_t spectrum_trace(void);
const char *spectrum_trace_name(uint8_t mode);

// Resolution bandwidth, 1 or 2 MHz. See spectrum.c for what 2 MHz means here.
void spectrum_set_rbw(uint8_t mhz);
uint8_t spectrum_rbw(void);

// Calibration offset in dB, clamped to SPECTRUM_CAL_MIN_DB..MAX_DB
void spectrum_set_cal(int8_t db);
int8_t spectrum_cal(void);

// A raw RSSI reading (-dBm, RSSISAMPLE units) as calibrated dBm
int spectrum_dbm(uint8_t rssi);

// Calibrated dBm of what the bars show at a channel: the live reading in peak
// mode, the trace level over the channel's floor otherwise. Returns false
// when there is no reading.
bool spectrum_level_dbm(uint8_t chan_index, int *dbm);

// Noise floor of the plot, RSSISAMPLE units: the mean tracked floor, or the
// fixed level when auto floor is off
uint8_t spectrum_floor_rssi(void);

// dB above the tracked noise floor for a channel, 0 when quiet
uint8_t spectrum_db(uint8_t chan_index);
uint8_t spectrum_max_db(uint8_t chan_index);

// The value the bars show for a channel in the current trace mode
uint8_t spectrum_trace_db(uint8_t chan_index);

// Channel under a screen column, and the middle column of a channel (or the
// nearest column when the band has more channels than the screen columns)
uint8_t spectrum_col_to_chan(int col);
int spectrum_chan_to_col(uint8_t chan_index);

uint16_t spectrum_history_rows(void);
uint32_t spectrum_sweeps(void);

// Strongest channel of the last sweep
uint8_t spectrum_strongest(void);

// Peak search on the displayed trace. rank 0 is the strongest peak, 1 the
// next lower one and so on. Only peaks that stand out of their surroundings
// count, so the ragged top of one WiFi channel is one peak, not ten.
// Returns false when there is no peak of that rank.
bool spectrum_find_peak(uint8_t rank, uint8_t *chan_index);

// Long window occupancy: share of samples above the busy threshold, averaged
// over about the last half minute. 0..100.
uint8_t spectrum_occupancy(uint8_t chan_index);

// Top n busiest channels by long window occupancy, fills idx, returns how
// many were written
uint8_t spectrum_top_busy(uint8_t *idx, uint8_t n);

#endif // PIXLA_SPECTRUM_H
