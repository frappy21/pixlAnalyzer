/**
 * Application level configuration: band plan, screen layout, timing and the
 * levels used to map RSSI samples to pixels.
 */
#ifndef PIXLA_APP_CONFIG_H
#define PIXLA_APP_CONFIG_H

// ---------------------------------------------------------------------------
// Band and sweep
// ---------------------------------------------------------------------------

// RADIO->FREQUENCY is an integer MHz offset. With MAP=Default the base is
// 2400MHz and the field reaches 2500MHz, with MAP=Low the base is 2360MHz.
#define FREQ_BASE_DEFAULT_MHZ 2400
#define FREQ_BASE_LOW_MHZ 2360
#define FREQ_CHANNEL_MAX 100 // RADIO->FREQUENCY accepts 0..100

// Widest sweep we support: 2360..2500MHz in 1MHz steps
#define SCAN_MAX_CHANNELS 141

// RSSI samples taken per channel per visit. The receiver stays in RXIDLE
// between them, so each sample costs about a microsecond.
#define SCAN_DWELL_SAMPLES_DEFAULT 32
#define SCAN_DWELL_SAMPLES_MAX 128

// ---------------------------------------------------------------------------
// Screen layout
// ---------------------------------------------------------------------------
#define STATUS_H 8                 // top status bar
#define SPECTRUM_TOP STATUS_H
#define SPECTRUM_H 24              // spectrum plot
#define RULER_Y (SPECTRUM_TOP + SPECTRUM_H) // channel ruler row
#define RULER_H 8
#define WATERFALL_START (RULER_Y + RULER_H)
#define WATERFALL_ROWS (DISP_H - WATERFALL_START) // 24 rows of history

// ---------------------------------------------------------------------------
// Levels
// ---------------------------------------------------------------------------

// RSSISAMPLE is a positive value in -dBm, so a smaller number is a stronger
// signal. The display works in "dB above the tracked noise floor".
#define RSSI_INVALID 127
#define NOISE_FLOOR_INIT 92
#define NOISE_FLOOR_MIN 60
#define NOISE_FLOOR_MAX 110
#define SIGNAL_MARGIN_DB 7    // dB above the floor before a channel counts as busy.
                              // Measured: pure noise peaks up to 6 dB (p90) above
                              // the tracked floor over a 32 sample visit
#define SPECTRUM_RANGE_DB 40  // dB mapped onto SPECTRUM_H pixels
#define DB_PER_GRID 10

// ---------------------------------------------------------------------------
// Timing
// ---------------------------------------------------------------------------
#define PEAK_DECAY_DB_PER_S 12   // spectrum peak hold decay
#define AUTO_DIM_MS 30000        // no button for this long: dim the display
#define AUTO_SLEEP_MS 300000     // no button for this long: go to sleep
#define LOW_BATTERY_MV 3400
#define CRITICAL_BATTERY_MV 3250

// DFU Magic Number (Standard Nordic SDK value)
#define BOOTLOADER_DFU_START 0xB1

// Cookie in GPREGRET2 telling the fresh boot that it came out of SYSTEM OFF
#define WAKE_COOKIE 0x5A

#endif // PIXLA_APP_CONFIG_H
