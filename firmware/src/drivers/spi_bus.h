/**
 * SPIM0 with EasyDMA, shared by the display and the 2MB SPI NOR flash.
 *
 * Both devices sit on P0.25/26 (MOSI/SCK) with their own chip selects, so all
 * access goes through this one owner. EasyDMA can only reach RAM, so the
 * helpers below stage short command bytes in a RAM scratch buffer.
 */
#ifndef PIXLA_SPI_BUS_H
#define PIXLA_SPI_BUS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

void spi_bus_init(void);
void spi_bus_uninit(void);

// Blocking write, len may exceed the 255 byte EasyDMA limit
void spi_bus_write(const uint8_t *data, size_t len);

// Blocking write followed by a read on the same chip select
void spi_bus_xfer(const uint8_t *tx, size_t tx_len, uint8_t *rx, size_t rx_len);

#endif // PIXLA_SPI_BUS_H
