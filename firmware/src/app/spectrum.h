/**
 * The display model: live trace, peak hold, max hold, a reference trace and
 * the waterfall history.
 *
 * History lives in RAM (the device has 58KB spare) and covers the whole
 * session, so the waterfall can be scrolled back to look at something that
 * already scrolled off the screen. Nothing is written to flash.
 */
#ifndef PIXLA_SPECTRUM_H
#define PIXLA_SPECTRUM_H

#include <stdbool.h>
#include <stdint.h>

#include "app_config.h"

// Rows of waterfall history kept in RAM, 32 bytes each
#define HISTORY_ROWS 384

void spectrum_reset(void);

// Folds the latest sweep into the traces and, every wf_decim sweeps, pushes a
// new waterfall row. Returns true when a new row was pushed.
bool spectrum_update(uint32_t now_ms);

void spectrum_draw(int marker_col);
void spectrum_draw_waterfall(uint16_t scroll_back);
void spectrum_draw_ruler(uint8_t plan, int marker_col);

void spectrum_snapshot_ref(void);
void spectrum_clear_ref(void);
void spectrum_clear_max(void);
bool spectrum_has_ref(void);

// dB above the tracked noise floor for a channel, 0 when quiet
uint8_t spectrum_db(uint8_t chan_index);
uint8_t spectrum_max_db(uint8_t chan_index);

// Channel under a screen column and back again
uint8_t spectrum_col_to_chan(int col);
int spectrum_chan_to_col(uint8_t chan_index);

uint16_t spectrum_history_rows(void);
uint32_t spectrum_sweeps(void);

// Strongest channel of the last sweep
uint8_t spectrum_strongest(void);

// Top n busiest channels by occupancy, fills idx, returns how many were written
uint8_t spectrum_top_busy(uint8_t *idx, uint8_t n);

#endif // PIXLA_SPECTRUM_H
