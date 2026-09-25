/**
 * Display driver for the two Pixl.js panels, over SPIM with EasyDMA.
 * ST7565/ST7567 by default, SH1106 when OLED_TYPE_SH1106 is defined.
 *
 * display_flush() only sends the pages that actually changed since the last
 * flush, so a mostly static screen costs almost nothing.
 */
#ifndef PIXLA_DISPLAY_H
#define PIXLA_DISPLAY_H

#include <stdbool.h>
#include <stdint.h>

#include "board_config.h"

// Frame buffer in the native page format of the controller:
// byte index = x + (y / 8) * DISP_W, bit index = y % 8
extern uint8_t g_frame_buffer[DISP_BUF_SIZE];

void display_init(void);
void display_uninit(void);

// Clears the frame buffer only, use display_flush() to push it to the panel
void display_clear(void);
void display_flush(void);
void display_flush_all(void); // ignore the dirty page tracking

// Drawn into the frame buffer by every flush, on top of whatever the screen
// drew (a short lived banner). NULL removes it. Screens redraw the whole
// frame, so the next frame after removal no longer has it.
void display_set_overlay(void (*draw)(void));

// 0..63 on both panels, mapped onto the controller's own contrast range
void display_set_contrast(uint8_t value);

// 0..255, PWM on the LCD backlight pin. The OLED ignores it.
void display_set_backlight(uint8_t level);

void display_set_inverted(bool inverted);

// Panel off (true) or on. The controller keeps its RAM, so switching back on
// shows the last frame at once. Used by the OLED screensaver.
void display_set_panel_off(bool off);

// OLED burn-in protection: every flush sends the frame moved dx pixels right
// and dy down (0 or 1 each), losing the last column and row. Does nothing on
// the LCD build.
void display_set_shift(uint8_t dx, uint8_t dy);

#endif // PIXLA_DISPLAY_H
