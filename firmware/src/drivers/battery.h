/**
 * LiPo gauge: oversampled SAADC on AIN0 plus the charger status pin.
 *
 * The state of charge comes from an interpolated discharge curve and is
 * clamped monotonic, so the reading never climbs back up while discharging.
 */
#ifndef PIXLA_BATTERY_H
#define PIXLA_BATTERY_H

#include <stdbool.h>
#include <stdint.h>

typedef struct
{
    uint16_t mv;      // filtered battery voltage in millivolts
    uint16_t mv_raw;  // last unfiltered reading, for diagnostics
    uint8_t percent;  // 0..100
    bool charging;    // charger pulls the status pin low
    bool valid;       // the reading is inside a plausible LiPo range
    bool low;         // below LOW_BATTERY_MV for two readings in a row
    bool critical;    // below CRITICAL_BATTERY_MV for several readings
    int16_t raw_adc;
} bat_status_t;

extern bat_status_t g_battery;

// Offset calibration, run once after reset before the first measurement
void battery_init(void);

// Blocking measurement, updates g_battery
void battery_update(void);

// Trim factor in per mille, 1000 = the nominal divider. Stored in settings so
// a user can calibrate against a multimeter.
void battery_set_calibration(uint16_t per_mille);

#endif // PIXLA_BATTERY_H
