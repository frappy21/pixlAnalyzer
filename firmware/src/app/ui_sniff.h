/** ESB sniffer rendering: the address list and the packet browser. */
#ifndef PIXLA_UI_SNIFF_H
#define PIXLA_UI_SNIFF_H

#include <stdint.h>

#include "esb_sniff.h"

// The SNIFF main screen: channel lock in the header, addresses below.
// lock_mhz 0 means hopping. a0_filter 0 = any, else the active A0 byte.
void ui_sniff_list(uint16_t lock_mhz, uint16_t hop_mhz, uint32_t locks, uint32_t decoded,
                   uint8_t a0_filter);

// Hex byte entry overlay, drawn while the user picks an A0 filter value
void ui_sniff_hex(uint8_t val);

// The packet browser: one packet of the ring, its header and payload
void ui_sniff_pkt(const esb_pkt_t *p, uint8_t pos, uint8_t count, bool captured);

#endif // PIXLA_UI_SNIFF_H
