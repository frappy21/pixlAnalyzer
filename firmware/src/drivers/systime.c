#include "nrf.h"

#include "systime.h"

#define RTC_TICKS_PER_S 32768
#define RTC_MASK 0x00FFFFFF // RTC counters are 24 bit

static uint32_t m_last_rtc;
static uint64_t m_ticks;

static void lfclk_start(void)
{
    // Always restart, never trust LFCLKSTAT: after the bootloader hands over it
    // reads "running" (RC) while the RTCs get no clock at all, which froze the
    // millisecond clock at 0 on hardware. Stopping is also the only way to
    // change the source to the crystal.
    NRF_CLOCK->TASKS_LFCLKSTOP = 1;
    for (uint32_t guard = 0; guard < 100000; guard++)
    {
        if (!(NRF_CLOCK->LFCLKSTAT & CLOCK_LFCLKSTAT_STATE_Msk))
            break;
    }

    NRF_CLOCK->LFCLKSRC = CLOCK_LFCLKSRC_SRC_Xtal << CLOCK_LFCLKSRC_SRC_Pos;
    NRF_CLOCK->EVENTS_LFCLKSTARTED = 0;
    NRF_CLOCK->TASKS_LFCLKSTART = 1;

    // The crystal needs up to ~0.5s to settle; fall back to the RC oscillator
    // rather than hanging here forever if the board has no LFXO fitted
    for (uint32_t guard = 0; guard < 8000000; guard++)
    {
        if (NRF_CLOCK->EVENTS_LFCLKSTARTED)
            return;
    }

    NRF_CLOCK->TASKS_LFCLKSTOP = 1;
    for (uint32_t guard = 0; guard < 100000; guard++)
    {
        if (!(NRF_CLOCK->LFCLKSTAT & CLOCK_LFCLKSTAT_STATE_Msk))
            break;
    }
    NRF_CLOCK->LFCLKSRC = CLOCK_LFCLKSRC_SRC_RC << CLOCK_LFCLKSRC_SRC_Pos;
    NRF_CLOCK->EVENTS_LFCLKSTARTED = 0;
    NRF_CLOCK->TASKS_LFCLKSTART = 1;
    for (uint32_t guard = 0; guard < 8000000; guard++)
    {
        if (NRF_CLOCK->EVENTS_LFCLKSTARTED)
            return;
    }
}

void systime_init(void)
{
    lfclk_start();

    NRF_RTC1->TASKS_STOP = 1;
    NRF_RTC1->PRESCALER = 0; // 32768Hz
    NRF_RTC1->EVTENCLR = 0xFFFFFFFF;
    NRF_RTC1->INTENCLR = 0xFFFFFFFF;
    NRF_RTC1->TASKS_CLEAR = 1;
    NRF_RTC1->TASKS_START = 1;

    m_last_rtc = 0;
    m_ticks = 0;

    // 1MHz free running counter for microsecond timestamps
    NRF_TIMER0->TASKS_STOP = 1;
    NRF_TIMER0->MODE = TIMER_MODE_MODE_Timer;
    NRF_TIMER0->BITMODE = TIMER_BITMODE_BITMODE_32Bit;
    NRF_TIMER0->PRESCALER = 4;
    NRF_TIMER0->TASKS_CLEAR = 1;
    NRF_TIMER0->TASKS_START = 1;

    // Let a pending interrupt wake __WFE even though we vector no ISRs
    SCB->SCR |= SCB_SCR_SEVONPEND_Msk;
}

uint32_t systime_ms(void)
{
    uint32_t now = NRF_RTC1->COUNTER & RTC_MASK;
    uint32_t delta = (now - m_last_rtc) & RTC_MASK;

    m_last_rtc = now;
    m_ticks += delta;

    // ms = ticks * 1000 / 32768, exactly, without a division
    return (uint32_t)((m_ticks * 125u) >> 12);
}

uint32_t systime_us(void)
{
    NRF_TIMER0->TASKS_CAPTURE[1] = 1;
    return NRF_TIMER0->CC[1];
}

void systime_idle(uint32_t ms)
{
    if (ms == 0)
        return;

    uint32_t ticks = (ms * RTC_TICKS_PER_S) / 1000;
    if (ticks < 2)
        ticks = 2;

    NRF_RTC1->CC[0] = (NRF_RTC1->COUNTER + ticks) & RTC_MASK;
    NRF_RTC1->EVENTS_COMPARE[0] = 0;
    NRF_RTC1->INTENSET = RTC_INTENSET_COMPARE0_Msk;

    // SEVONPEND is set, so the pending RTC1 interrupt wakes us without an ISR
    __SEV();
    __WFE();
    __WFE();

    NRF_RTC1->INTENCLR = RTC_INTENCLR_COMPARE0_Msk;
    NRF_RTC1->EVENTS_COMPARE[0] = 0;
    NVIC_ClearPendingIRQ(RTC1_IRQn);
}
