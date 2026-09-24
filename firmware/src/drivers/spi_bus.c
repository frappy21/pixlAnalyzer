#include <string.h>

#include "nrf.h"
#include "nrf_gpio.h"

#include "board_config.h"
#include "spi_bus.h"

// nRF52832 SPIM EasyDMA counters are 8 bit, so one transfer is at most 255 bytes
#define SPI_CHUNK_MAX 255

static uint8_t m_scratch[SPI_CHUNK_MAX];
static uint8_t m_sink[SPI_CHUNK_MAX];

void spi_bus_init(void)
{
    nrf_gpio_cfg_output(PIN_LCD_SCL);
    nrf_gpio_cfg_output(PIN_LCD_MOSI);
    nrf_gpio_pin_clear(PIN_LCD_SCL);
    nrf_gpio_cfg_input(PIN_LCD_MISO, NRF_GPIO_PIN_NOPULL);

    NRF_SPIM0->PSEL.SCK = PIN_LCD_SCL;
    NRF_SPIM0->PSEL.MOSI = PIN_LCD_MOSI;
    NRF_SPIM0->PSEL.MISO = PIN_LCD_MISO;
    NRF_SPIM0->FREQUENCY = SPIM_FREQUENCY_FREQUENCY_M8;
    NRF_SPIM0->CONFIG = (SPIM_CONFIG_ORDER_MsbFirst << SPIM_CONFIG_ORDER_Pos) |
                        (SPIM_CONFIG_CPHA_Leading << SPIM_CONFIG_CPHA_Pos) |
                        (SPIM_CONFIG_CPOL_ActiveHigh << SPIM_CONFIG_CPOL_Pos);
    NRF_SPIM0->ORC = 0xFF;
    NRF_SPIM0->ENABLE = SPIM_ENABLE_ENABLE_Enabled << SPIM_ENABLE_ENABLE_Pos;
}

void spi_bus_uninit(void)
{
    NRF_SPIM0->ENABLE = 0;
    NRF_SPIM0->PSEL.SCK = 0xFFFFFFFF;
    NRF_SPIM0->PSEL.MOSI = 0xFFFFFFFF;
    NRF_SPIM0->PSEL.MISO = 0xFFFFFFFF;
    nrf_gpio_cfg_default(PIN_LCD_SCL);
    nrf_gpio_cfg_default(PIN_LCD_MOSI);
    nrf_gpio_cfg_default(PIN_LCD_MISO);
}

static void spi_run(const uint8_t *tx, size_t tx_len, uint8_t *rx, size_t rx_len)
{
    NRF_SPIM0->TXD.PTR = (uint32_t)tx;
    NRF_SPIM0->TXD.MAXCNT = tx_len;
    NRF_SPIM0->RXD.PTR = (uint32_t)rx;
    NRF_SPIM0->RXD.MAXCNT = rx_len;

    NRF_SPIM0->EVENTS_END = 0;
    NRF_SPIM0->TASKS_START = 1;

    // Bounded wait: a full 255 byte transfer at 8MHz takes ~260us, so this
    // loop can only spin if the peripheral is wedged
    for (uint32_t guard = 0; guard < 2000000; guard++)
    {
        if (NRF_SPIM0->EVENTS_END)
            break;
    }
    NRF_SPIM0->EVENTS_END = 0;
}

void spi_bus_write(const uint8_t *data, size_t len)
{
    while (len)
    {
        size_t chunk = len > SPI_CHUNK_MAX ? SPI_CHUNK_MAX : len;

        // EasyDMA can only read from RAM, so stage anything else in scratch
        if ((uint32_t)data < 0x20000000)
        {
            memcpy(m_scratch, data, chunk);
            spi_run(m_scratch, chunk, m_sink, 0);
        }
        else
        {
            spi_run(data, chunk, m_sink, 0);
        }

        data += chunk;
        len -= chunk;
    }
}

void spi_bus_xfer(const uint8_t *tx, size_t tx_len, uint8_t *rx, size_t rx_len)
{
    if (tx_len > SPI_CHUNK_MAX)
        tx_len = SPI_CHUNK_MAX;
    if (rx_len > SPI_CHUNK_MAX)
        rx_len = SPI_CHUNK_MAX;

    memcpy(m_scratch, tx, tx_len);
    spi_run(m_scratch, tx_len, m_sink, tx_len + rx_len);
    if (rx_len)
        memcpy(rx, &m_sink[tx_len], rx_len);
}
