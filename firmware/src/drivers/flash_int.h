/**
 * Internal flash access for persisted settings.
 *
 * The application slot ends at 0x74000 and the bootloader starts at 0x77000,
 * so the three pages in between belong to nobody. We use the last one, which
 * keeps settings clear of both the DFU image and the pixl.js filesystem in the
 * external flash chip.
 */
#ifndef PIXLA_FLASH_INT_H
#define PIXLA_FLASH_INT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define FLASH_INT_PAGE_SIZE 4096
#define SETTINGS_PAGE_ADDR 0x00076000

bool flash_int_erase_page(uint32_t addr);

// len is rounded up to whole 32 bit words, addr must be word aligned
bool flash_int_write(uint32_t addr, const void *data, size_t len);

#endif // PIXLA_FLASH_INT_H
