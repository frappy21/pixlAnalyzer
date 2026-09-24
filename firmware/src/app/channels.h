/**
 * Channel plans of the technologies that share the band, and the scoring that
 * turns an occupancy sweep into "use channel 11".
 */
#ifndef PIXLA_CHANNELS_H
#define PIXLA_CHANNELS_H

#include <stdbool.h>
#include <stdint.h>

typedef enum
{
    PLAN_NONE = 0,
    PLAN_WIFI,   // 802.11b/g/n, 13 channels, 20MHz wide
    PLAN_BLE,    // advertising channels 37/38/39
    PLAN_ZIGBEE, // IEEE 802.15.4 channels 11..26
    PLAN_COUNT
} chan_plan_t;

typedef struct
{
    uint16_t center_mhz;
    uint8_t half_width; // MHz either side of the centre
    uint8_t number;     // channel number in its own plan
} chan_mark_t;

const char *channels_plan_name(uint8_t plan);
uint8_t channels_plan_count(uint8_t plan);
bool channels_plan_get(uint8_t plan, uint8_t index, chan_mark_t *out);

// Short label for a frequency, e.g. "W6", "B38", "Z15". Empty if nothing known.
const char *channels_label(uint16_t mhz);

// Mean occupancy (0..255) over the frequencies a channel occupies
uint8_t channels_occupancy(uint8_t plan, uint8_t index);

// Best WiFi channel out of 1/6/11 by occupancy, with its score
uint8_t channels_best_wifi(uint8_t *score_out);

#endif // PIXLA_CHANNELS_H
