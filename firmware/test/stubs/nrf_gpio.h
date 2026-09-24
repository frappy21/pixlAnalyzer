#pragma once
#define NRF_GPIO_PIN_PULLUP 3
static inline void nrf_gpio_cfg_input(unsigned pin, int pull) { (void)pin; (void)pull; }
extern int host_pin_level(unsigned pin);
static inline int nrf_gpio_pin_read(unsigned pin) { return host_pin_level(pin); }
