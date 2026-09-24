#include <string.h>

#include "nrf_delay.h"
#include "nrf_gpio.h"

#include "board_config.h"
#include "flash_ext.h"
#include "power.h"
#include "spi_bus.h"

#define CMD_WRITE_ENABLE 0x06
#define CMD_READ_STATUS 0x05
#define CMD_READ_DATA 0x03
#define CMD_PAGE_PROGRAM 0x02
#define CMD_SECTOR_ERASE 0x20
#define CMD_JEDEC_ID 0x9F
#define CMD_DEEP_SLEEP 0xB9
#define CMD_WAKE 0xAB

#define STATUS_WIP 0x01

#define CHUNK 192 // stays inside the 255 byte EasyDMA limit with the header

static uint32_t m_jedec;
static uint32_t m_size;
static uint8_t m_buf[CHUNK + 4];

static void cs_low(void) { nrf_gpio_pin_clear(PIN_FLASH_CS); }
static void cs_high(void) { nrf_gpio_pin_set(PIN_FLASH_CS); }

static void cmd_only(uint8_t cmd)
{
    cs_low();
    spi_bus_write(&cmd, 1);
    cs_high();
}

static uint8_t read_status(void)
{
    uint8_t tx = CMD_READ_STATUS;
    uint8_t rx = 0xFF;

    cs_low();
    spi_bus_xfer(&tx, 1, &rx, 1);
    cs_high();
    return rx;
}

static bool wait_ready(uint32_t timeout_ms)
{
    for (uint32_t i = 0; i < timeout_ms * 10; i++)
    {
        if ((read_status() & STATUS_WIP) == 0)
            return true;
        nrf_delay_us(100);
        if ((i % 1000) == 0)
            power_watchdog_feed();
    }
    return false;
}

bool flash_ext_init(void)
{
    nrf_gpio_cfg_output(PIN_FLASH_CS);
    cs_high();

    flash_ext_wake();

    uint8_t tx = CMD_JEDEC_ID;
    uint8_t rx[3] = {0, 0, 0};
    cs_low();
    spi_bus_xfer(&tx, 1, rx, 3);
    cs_high();

    m_jedec = ((uint32_t)rx[0] << 16) | ((uint32_t)rx[1] << 8) | rx[2];

    // A missing or asleep chip reads as all ones or all zeroes
    if (m_jedec == 0x000000 || m_jedec == 0xFFFFFF)
    {
        m_size = 0;
        return false;
    }

    // The third ID byte is log2 of the capacity in bytes on every part we care about
    uint8_t cap = rx[2];
    m_size = (cap >= 0x10 && cap <= 0x19) ? (1u << cap) : 0;
    return m_size != 0;
}

bool flash_ext_present(void) { return m_size != 0; }
uint32_t flash_ext_jedec_id(void) { return m_jedec; }
uint32_t flash_ext_size(void) { return m_size; }

bool flash_ext_read(uint32_t addr, void *dst, size_t len)
{
    if (!m_size || addr + len > m_size)
        return false;

    uint8_t *out = (uint8_t *)dst;
    while (len)
    {
        size_t chunk = len > CHUNK ? CHUNK : len;
        uint8_t hdr[4] = {CMD_READ_DATA, (addr >> 16) & 0xFF, (addr >> 8) & 0xFF, addr & 0xFF};

        cs_low();
        spi_bus_xfer(hdr, 4, out, chunk);
        cs_high();

        addr += chunk;
        out += chunk;
        len -= chunk;
    }
    return true;
}

bool flash_ext_erase_sector(uint32_t addr)
{
    if (!m_size || addr >= m_size)
        return false;

    addr &= ~(uint32_t)(FLASH_EXT_SECTOR_SIZE - 1);

    cmd_only(CMD_WRITE_ENABLE);

    uint8_t hdr[4] = {CMD_SECTOR_ERASE, (addr >> 16) & 0xFF, (addr >> 8) & 0xFF, addr & 0xFF};
    cs_low();
    spi_bus_write(hdr, 4);
    cs_high();

    return wait_ready(1000); // datasheet worst case is ~400ms
}

bool flash_ext_write(uint32_t addr, const void *src, size_t len)
{
    if (!m_size || addr + len > m_size)
        return false;

    const uint8_t *in = (const uint8_t *)src;
    while (len)
    {
        // A page program may not cross a 256 byte page boundary
        size_t space = FLASH_EXT_PAGE_SIZE - (addr % FLASH_EXT_PAGE_SIZE);
        size_t chunk = len < space ? len : space;
        if (chunk > CHUNK)
            chunk = CHUNK;

        cmd_only(CMD_WRITE_ENABLE);

        m_buf[0] = CMD_PAGE_PROGRAM;
        m_buf[1] = (addr >> 16) & 0xFF;
        m_buf[2] = (addr >> 8) & 0xFF;
        m_buf[3] = addr & 0xFF;
        memcpy(&m_buf[4], in, chunk);

        cs_low();
        spi_bus_write(m_buf, chunk + 4);
        cs_high();

        if (!wait_ready(50))
            return false;

        addr += chunk;
        in += chunk;
        len -= chunk;
    }
    return true;
}

void flash_ext_sleep(void)
{
    cmd_only(CMD_DEEP_SLEEP);
}

void flash_ext_wake(void)
{
    cmd_only(CMD_WAKE);
    nrf_delay_us(50);
}
