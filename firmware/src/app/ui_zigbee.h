/** IEEE 802.15.4 (Zigbee / Thread) receiver rendering: overview and one PAN. */
#ifndef PIXLA_UI_ZIGBEE_H
#define PIXLA_UI_ZIGBEE_H

#include <stdint.h>

#include "zb_rx.h"

// Channel activity 11..26, totals and the busiest PANs. lock = 0 while
// hopping, else the channel listened to.
void ui_zigbee_main(uint8_t lock);

// Detail card of one PAN, position pos of n in the list. The address list is
// paged; returns the number of pages (page is taken modulo that).
uint8_t ui_zigbee_pan(const zb_pan_t *pan, uint8_t pos, uint8_t n, uint8_t page);

#endif // PIXLA_UI_ZIGBEE_H
