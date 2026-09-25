/** ESB transmitter rendering: the running view. */
#ifndef PIXLA_UI_ESB_TX_H
#define PIXLA_UI_ESB_TX_H

#include <stdbool.h>
#include <stdint.h>

// While transmitting: mode (0 replay, 1 inject), channel, PHY, packets sent
// and the auto stop countdown
void ui_esb_tx_active(uint8_t mode, uint16_t mhz, uint8_t rate, uint32_t sent,
                      uint32_t remaining_ms);

#endif // PIXLA_UI_ESB_TX_H
