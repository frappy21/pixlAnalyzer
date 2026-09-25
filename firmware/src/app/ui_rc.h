/**
 * Rendering for the RC screens: the stick dashboard and the emulator's
 * configuration rows. Rendering only, no input handling.
 */
#ifndef PIXLA_UI_RC_H
#define PIXLA_UI_RC_H

#include <stdbool.h>
#include <stdint.h>

#include "rc_proto.h"

// The dash: what the screen knows about the link it is following
typedef struct
{
    rc_proto_t proto;      // RC_PROTO_UNKNOWN for the generic view
    uint16_t mhz;          // the channel the link was last heard on
    bool locked;           // packets are coming in right now
    uint8_t rssi;          // strongest, in -dBm
    uint8_t pps;           // decoded packets per second (saturated)
    uint8_t reacquires;    // how often the hop follow had to resweep
    uint8_t flags;         // RC_FLAG_* of the last decoded frame
    rc_sticks_t sticks;    // the last decoded sticks
    bool bind;             // the link is in its bind phase right now

    // Generic view: which payload bytes move, and their live range
    uint32_t live_mask;
    uint8_t centre[RC_TRACK_MAX];
    uint8_t value[RC_TRACK_MAX];
} rc_dash_view_t;

void ui_rc_dash(const rc_dash_view_t *view);

// One stick row: a bar with a centre tick for the signed sticks, a filled
// bar for the throttle
void ui_rc_stick_row(int y, const char *name, int8_t value, bool signed_stick);

// The emulator configuration: rows of label + value, one selected
typedef struct
{
    rc_proto_t proto;
    rc_sticks_t sticks;  // the values being edited
    uint8_t power;       // TX_POWER_*
    uint16_t count;      // packets per burst
    uint16_t mhz;        // the channel to transmit on
    uint8_t rows;        // row count, fixed layout
    uint8_t selected;
    uint8_t remaining_s; // while running
    bool running;
    bool has_capture;    // a control packet was captured to build from
} rc_tx_view_t;

void ui_rc_tx(const rc_tx_view_t *view);

#endif // PIXLA_UI_RC_H
