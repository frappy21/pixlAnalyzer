/**
 * The status LED on P0.31 (active low), unused by the stock firmware.
 * Used here as an eyes-free signal indicator while hunting an emitter.
 */
#ifndef PIXLA_LED_H
#define PIXLA_LED_H

#include <stdbool.h>
#include <stdint.h>

void led_init(void);
void led_set(bool on);

// Blink at the given rate. 0 turns the LED off, 255 holds it on.
// Call led_update() from the main loop to keep the pattern running.
void led_set_rate(uint8_t hits_per_second);
void led_update(uint32_t now_ms);

void led_off(void);

#endif // PIXLA_LED_H
