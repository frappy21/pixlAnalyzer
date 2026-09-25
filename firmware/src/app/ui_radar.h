/**
 * Rendering for the RADAR screen (signal list, hunt and microwave modes).
 * Rendering only, no input handling.
 */
#ifndef PIXLA_UI_RADAR_H
#define PIXLA_UI_RADAR_H

#include <stdbool.h>
#include <stdint.h>

#include "radar.h"

// What the radar screen wants drawn
typedef struct
{
    uint8_t mode; // 0 scan, 1 hunt, 2 microwave
    bool hunt_armed;

    // The activity strip: per channel dB above the floor, quantised to
    // 0..255, 84 entries for 2400..2483
    const uint8_t *strip;
    uint8_t strip_len;

    // The signal list
    uint8_t selected;
    const radar_work_t *work;

    // Microwave mode
    int8_t mw_level;    // dB above the floor
    const char *mw_verdict;
    const uint8_t *mw_trend; // recent levels, older first
    uint8_t mw_trend_len;
} radar_view_t;

const char *radar_mode_name(uint8_t mode);

void ui_radar(const radar_view_t *view);

#endif // PIXLA_UI_RADAR_H
