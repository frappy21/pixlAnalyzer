#include <string.h>

#include "nrf.h"

#include "flash_int.h"
#include "power.h"

static bool nvmc_wait(void)
{
    for (uint32_t guard = 0; guard < 20000000; guard++)
    {
        if (NRF_NVMC->READY == NVMC_READY_READY_Ready)
            return true;
        power_watchdog_feed();
    }
    return false;
}

bool flash_int_erase_page(uint32_t addr)
{
    if (addr % FLASH_INT_PAGE_SIZE)
        return false;

    NRF_NVMC->CONFIG = NVMC_CONFIG_WEN_Een << NVMC_CONFIG_WEN_Pos;
    if (!nvmc_wait())
        goto fail;

    NRF_NVMC->ERASEPAGE = addr;
    if (!nvmc_wait())
        goto fail;

    NRF_NVMC->CONFIG = NVMC_CONFIG_WEN_Ren << NVMC_CONFIG_WEN_Pos;
    return nvmc_wait();

fail:
    NRF_NVMC->CONFIG = NVMC_CONFIG_WEN_Ren << NVMC_CONFIG_WEN_Pos;
    return false;
}

bool flash_int_write(uint32_t addr, const void *data, size_t len)
{
    if (addr % 4)
        return false;

    const uint8_t *src = (const uint8_t *)data;
    size_t words = (len + 3) / 4;

    NRF_NVMC->CONFIG = NVMC_CONFIG_WEN_Wen << NVMC_CONFIG_WEN_Pos;
    if (!nvmc_wait())
        goto fail;

    for (size_t i = 0; i < words; i++)
    {
        uint32_t word = 0xFFFFFFFF;
        size_t left = len - i * 4;
        memcpy(&word, src + i * 4, left > 4 ? 4 : left);

        *(volatile uint32_t *)(addr + i * 4) = word;
        if (!nvmc_wait())
            goto fail;
    }

    NRF_NVMC->CONFIG = NVMC_CONFIG_WEN_Ren << NVMC_CONFIG_WEN_Pos;
    return nvmc_wait();

fail:
    NRF_NVMC->CONFIG = NVMC_CONFIG_WEN_Ren << NVMC_CONFIG_WEN_Pos;
    return false;
}
