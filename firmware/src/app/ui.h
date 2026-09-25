/**
 * Shared screen furniture (title, battery readout, lists, messages) and the
 * spectrum family of screens. Rendering only, no input handling. Screens with
 * their own module draw through ui_<module>.h.
 */
#ifndef PIXLA_UI_H
#define PIXLA_UI_H

#include <stdbool.h>
#include <stdint.h>

#include "classify.h"

typedef enum
{
    TOOL_MARK = 0, // move the marker
    TOOL_PEAK,     // marker to the strongest peak, then the next lower ones
    TOOL_SPAN,     // zoom in and out around the marker
    TOOL_WFALL,    // waterfall speed
    TOOL_SCROLL,   // scroll back through the history
    TOOL_COUNT
} scanner_tool_t;

// What the scanner screen shows below the status bar
typedef enum
{
    LAYOUT_SPLIT = 0,  // spectrum, ruler, waterfall
    LAYOUT_SPECTRUM,   // a spectrum twice as tall, ruler at the bottom
    LAYOUT_WATERFALL,  // ruler on top, a waterfall twice as tall
    LAYOUT_OCC,        // channel occupancy bar chart (WiFi + BLE adv)
    LAYOUT_COUNT
} scanner_layout_t;

typedef struct
{
    uint8_t tool;
    int marker_col;
    uint16_t scroll_back;
    bool frozen;
    uint8_t plan;       // channel overlay currently shown
    uint32_t sweeps_s;  // sweeps per second
    uint8_t layout;     // scanner_layout_t
    uint16_t delta_mhz; // delta reference marker, 0 = off
    bool tool_hint;     // name the tool where the delta readout would be
    bool alarm;         // interference alarm is up
    uint8_t alarm_chan; // channel that raised it
    uint8_t alarm_db;   // and how far above its floor
} scanner_view_t;

// Title in the big font plus the rule under it
void ui_title(const char *title);

// Battery voltage, micro font, right aligned on the top row: "3.87V",
// "+4.12V" while charging, "?.??V" when the reading is implausible
void ui_battery(void);

void ui_boot_screen(void);
void ui_power_on_gate(void);
void ui_message(const char *line1, const char *line2, uint32_t hold_ms);

void ui_scanner(const scanner_view_t *view);

// Waterfall rows a layout shows, 0 for none
uint8_t ui_scanner_waterfall_rows(uint8_t layout);
const char *ui_layout_name(uint8_t layout);

// Generic vertical list, used by the menu and the settings screen
void ui_list(const char *title, const char *const *items, uint8_t count, uint8_t selected,
             const char *const *values);

void ui_top_channels(void);
void ui_meter(uint16_t mhz, uint8_t rssi, uint8_t db, const uint8_t *trend, uint8_t trend_len);
void ui_identify(uint16_t mhz, const verdict_t *verdict, bool ble_confirmed, uint16_t ble_packets);
void ui_identify_progress(uint16_t mhz, uint8_t percent);
void ui_low_battery(void);

#endif // PIXLA_UI_H
