/**
 * The lab jammer: a modulated noise transmitter for immunity testing of
 * self owned receivers. Three modes:
 *
 *  NOISE  - random-data GFSK on one channel: to the target link the
 *           channel is full of CRC-failing frames and demodulator noise.
 *  SWEEP  - the same, stepping through the band so a hopper cannot dodge.
 *  WIFI   - parked on a WiFi channel centre, the noisiest neighbour an AP
 *           or a WiFi video link can get in this band.
 *
 * The safety pattern is the one every TX screen here follows: the lowest
 * power by default, a hold to start, any button stops it, a hard time
 * limit, and leave() always stops. The screen says what it is for: self
 * owned devices in a lab, ideally inside a Faraday bag or a screened box.
 */
#ifndef PIXLA_JAM_H
#define PIXLA_JAM_H

#include <stdbool.h>
#include <stdint.h>

#include "tx_test.h" // the shared TX power steps

// Longest unattended run
#define JAM_MAX_MS 30000u

// Jam modes
typedef enum
{
    JAM_NOISE = 0, // one channel, random data
    JAM_SWEEP,     // steps through 2400..2483
    JAM_WIFI,      // parked on a WiFi channel centre
    JAM_MODE_COUNT
} jam_mode_t;

// WiFi channel 1..14 -> centre frequency in MHz
uint16_t jam_wifi_channel_mhz(uint8_t wifi_chan);

typedef struct
{
    uint8_t mode;    // jam_mode_t
    uint16_t mhz;    // NOISE: the channel to jam; WIFI: overridden by wifi_chan
    uint8_t wifi_chan;
    uint8_t rate;    // 1 = 1 Mbit, 2 = 2 Mbit (2 fills the channel faster)
    uint8_t power;   // tx_power_t index
} jam_cfg_t;

// Arms the jammer (no radio yet) and returns true. The screen must then
// pump it from its tick, like the other TX screens.
bool jam_start(const jam_cfg_t *cfg);

// Stops and hands the radio back. Idempotent; call from leave() too.
void jam_stop(void);

// True while armed (also true while actively transmitting)
bool jam_active(void);

// One burst of noise packets, short enough that the buttons stay
// responsive. The screen tick calls this while armed.
void jam_pump(void);

// Auto stop check and the countdown for the screen
void jam_update(uint32_t now_ms);
uint32_t jam_remaining_ms(uint32_t now_ms);

// Statistics for the screen
uint32_t jam_packets_sent(void);
uint16_t jam_current_mhz(void);

#endif // PIXLA_JAM_H
