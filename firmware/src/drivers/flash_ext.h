/**
 * 2MB SPI NOR flash (GD25Q16C / ZD25WQ16B) on P0.18, sharing the display bus.
 *
 * The stock pixl.js firmware keeps a SPIFFS filesystem here with the user's
 * amiibo dumps, so this driver never writes below FLASH_EXT_SAFE_BASE and the
 * logger asks for consent before claiming space.
 */
#ifndef PIXLA_FLASH_EXT_H
#define PIXLA_FLASH_EXT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define FLASH_EXT_SECTOR_SIZE 4096
#define FLASH_EXT_PAGE_SIZE 256

// Upper half of the chip. The pixl.js filesystem starts at 0 and grows up, so
// staying above this keeps a 1MB buffer between us and the user's data.
#define FLASH_EXT_SAFE_BASE 0x100000

bool flash_ext_init(void);
bool flash_ext_present(void);
uint32_t flash_ext_jedec_id(void);
uint32_t flash_ext_size(void);

bool flash_ext_read(uint32_t addr, void *dst, size_t len);
bool flash_ext_write(uint32_t addr, const void *src, size_t len);
bool flash_ext_erase_sector(uint32_t addr);

// Deep power down between logging bursts, ~1uA instead of ~15uA idle
void flash_ext_sleep(void);
void flash_ext_wake(void);

#endif // PIXLA_FLASH_EXT_H
