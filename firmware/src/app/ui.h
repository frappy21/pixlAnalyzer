/** All screens of the application. Rendering only, no input handling. */
#ifndef PIXLA_UI_H
#define PIXLA_UI_H

#include <stdbool.h>
#include <stdint.h>

#include "ble_scan.h"
#include "classify.h"
#include "esb_scan.h"

typedef enum
{
    TOOL_MARK = 0, // move the marker
    TOOL_SPAN,     // zoom in and out around the marker
    TOOL_WFALL,    // waterfall speed
    TOOL_SCROLL,   // scroll back through the history
    TOOL_COUNT
} scanner_tool_t;

typedef struct
{
    uint8_t tool;
    int marker_col;
    uint16_t scroll_back;
    bool frozen;
    uint8_t plan;      // channel overlay currently shown
    uint32_t sweeps_s; // sweeps per second
} scanner_view_t;

void ui_boot_screen(void);
void ui_power_on_gate(void);
void ui_message(const char *line1, const char *line2, uint32_t hold_ms);

void ui_scanner(const scanner_view_t *view);

// Generic vertical list, used by the menu and the settings screen
void ui_list(const char *title, const char *const *items, uint8_t count, uint8_t selected,
             const char *const *values);

void ui_info(uint32_t uptime_s, uint32_t sweeps, uint16_t history_rows);
void ui_top_channels(void);
void ui_meter(uint16_t mhz, uint8_t rssi, uint8_t db, const uint8_t *trend, uint8_t trend_len);
void ui_identify(uint16_t mhz, const verdict_t *verdict, bool ble_confirmed, uint16_t ble_packets);
void ui_identify_progress(uint16_t mhz, uint8_t percent);
void ui_ble_list(uint8_t selected, uint32_t packets);
void ui_ble_detail(const ble_dev_t *dev);
void ui_esb_list(uint32_t total);
void ui_tx_confirm(uint16_t mhz, uint8_t power);
void ui_tx_active(uint16_t mhz, uint8_t power, uint32_t remaining_ms);
void ui_low_battery(void);

#endif // PIXLA_UI_H
