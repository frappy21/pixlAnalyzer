/** Transmitter test rendering: confirmation page and active carrier. */
#ifndef PIXLA_UI_TX_H
#define PIXLA_UI_TX_H

#include <stdint.h>

void ui_tx_confirm(uint16_t mhz, uint8_t power);
void ui_tx_active(uint16_t mhz, uint8_t power, uint32_t remaining_ms);

#endif // PIXLA_UI_TX_H
