/**
 * Passive ANT+ broadcast receiver on 2457 MHz.
 *
 * Correlates on the ANT sync byte (0xA4) + standard broadcast length (0x08),
 * so only 8-data-byte broadcast messages (type 0x4E) are captured. This covers
 * all open ANT+ sensor profiles (HR, cadence, speed, power) which all use the
 * 8-byte broadcast format and the public network key.
 *
 * The radio is owned for the duration of ant_rx_run(); call scanner_init()
 * afterwards to return it to sweep mode.
 */
#ifndef PIXLA_ANT_RX_H
#define PIXLA_ANT_RX_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Maximum devices tracked per session
#define ANT_MAX_DEVICES 8

// ANT+ device profile hints, inferred from page number + value range
typedef enum
{
    ANT_PROF_UNKNOWN = 0,
    ANT_PROF_HR,      // heart rate monitor, page 0x04, data[6] = bpm
    ANT_PROF_POWER,   // power meter, page 0x10/0x11, data[6..7] = watts
} ant_profile_t;

typedef struct
{
    uint8_t channel;     // ANT channel number from the frame
    uint8_t page;        // data byte 0 (page number)
    uint8_t raw[8];      // 8 data bytes as received
    uint8_t rssi;        // -dBm of latest frame (0 = never seen)
    uint32_t last_ms;    // systime_ms() of last frame
    uint32_t frames;     // total frames from this channel
    uint8_t profile;     // ant_profile_t (best guess)
    uint16_t hr_bpm;     // valid when profile == ANT_PROF_HR
    uint16_t power_w;    // valid when profile == ANT_PROF_POWER
} ant_dev_t;

// Place the device table in a caller-provided buffer (the arena).
// Returns false if the buffer is too small.
bool ant_rx_init(void *mem, size_t size);

// Listen on 2457 MHz for up to window_ms, decoding every ANT broadcast frame.
// Returns the number of frames decoded.
uint16_t ant_rx_run(uint32_t window_ms);

// Number of distinct channels seen since ant_rx_init()
uint8_t ant_rx_count(void);

// Device record at index (0..count-1); NULL if out of range
const ant_dev_t *ant_rx_device(uint8_t index);

// ANT XOR checksum over a full message buffer (excludes sync/len already
// consumed as the address; reconstructs them from known constants).
// data points to the 11 post-address bytes: [type][ch][d0..d7][checksum].
// Returns true when the checksum byte makes the XOR come out to zero.
bool ant_frame_valid(const uint8_t *data);

#endif // PIXLA_ANT_RX_H
