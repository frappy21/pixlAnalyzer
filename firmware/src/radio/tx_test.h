/**
 * Unmodulated test carrier, for antenna work and two device range checks.
 *
 * This transmits. It is off by default, needs an explicit confirmation, starts
 * at the lowest power and stops itself after TX_TEST_MAX_MS so it can never be
 * left running by accident. It does not imitate any device or protocol.
 */
#ifndef PIXLA_TX_TEST_H
#define PIXLA_TX_TEST_H

#include <stdbool.h>
#include <stdint.h>

#define TX_TEST_MAX_MS 30000

typedef enum
{
    TX_POWER_MIN = 0, // -40 dBm
    TX_POWER_LOW,     // -20 dBm
    TX_POWER_MID,     // -8 dBm
    TX_POWER_MAX,     // 0 dBm
    TX_POWER_COUNT
} tx_power_t;

bool tx_test_start(uint16_t mhz, uint8_t power);
void tx_test_stop(void);
bool tx_test_active(void);

// Call from the main loop, stops the carrier when the time limit is reached
void tx_test_update(uint32_t now_ms);

uint32_t tx_test_remaining_ms(uint32_t now_ms);
const char *tx_power_name(uint8_t power);

// The RADIO_TXPOWER register value of one step, shared with the other
// transmitters (esb_tx, ble_beacon)
uint32_t tx_power_reg(uint8_t power);

#endif // PIXLA_TX_TEST_H
