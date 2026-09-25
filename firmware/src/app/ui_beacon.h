/** Beacon transmitter rendering. */
#ifndef PIXLA_UI_BEACON_H
#define PIXLA_UI_BEACON_H

#include <stdint.h>

// The configuration: preset name and description, the adjustable rows
void ui_beacon_config(uint8_t type, uint8_t sel, uint8_t power, uint16_t interval_ms, bool edit);

// While transmitting
void ui_beacon_active(uint8_t type, uint32_t sent, uint32_t remaining_ms);

#endif // PIXLA_UI_BEACON_H
