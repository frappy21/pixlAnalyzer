/** Info screen rendering. */
#ifndef PIXLA_UI_INFO_H
#define PIXLA_UI_INFO_H

#include <stdbool.h>
#include <stdint.h>

#include "power.h"

typedef enum
{
    INFO_PAGE_SYSTEM = 0, // firmware, reset, uptime, temperature, RAM
    INFO_PAGE_POWER,      // battery, flash, sweep statistics
    INFO_PAGE_CRASH,      // last crash record and the crash test
    INFO_PAGE_COUNT
} info_page_t;

typedef struct
{
    uint8_t page;         // info_page_t
    uint32_t uptime_s;
    uint32_t sweeps;
    uint16_t history_rows;
    bool test_overflow;   // crash test kind: stack overflow instead of a bad instruction
} info_view_t;

void ui_info(const info_view_t *view);

// The lines of a crash record, from y down (five micro font rows). Shared
// with the report shown at boot.
void ui_info_crash_lines(const power_crash_t *crash, int y);

#endif // PIXLA_UI_INFO_H
