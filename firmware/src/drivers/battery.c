#include "nrf.h"
#include "nrf_delay.h"
#include "nrf_gpio.h"
#include "nrf_saadc.h"

#include "app_config.h"
#include "battery.h"
#include "board_config.h"

// Anything outside this range is a broken measurement, not a flat cell
#define BAT_SANE_MIN_MV 2500
#define BAT_SANE_MAX_MV 4600

bat_status_t g_battery = {0};

static uint16_t m_cal_per_mille = 1000;
static uint8_t m_low_streak;
static uint32_t m_filter_mv; // exponential average, 0 until the first reading

// Discharge curve of a single cell LiPo under light load
static const struct
{
    uint16_t mv;
    uint8_t percent;
} soc_curve[] = {
    {4200, 100}, {4100, 92}, {4000, 85}, {3900, 75}, {3850, 65}, {3800, 55},
    {3750, 45},  {3700, 35}, {3650, 25}, {3600, 18}, {3500, 10}, {3400, 4},
    {3300, 0},
};

#define SOC_POINTS (sizeof(soc_curve) / sizeof(soc_curve[0]))
#define BAT_OVERSAMPLE 8

static void saadc_setup(void)
{
    nrf_gpio_cfg_input(PIN_ADC_INPUT, NRF_GPIO_PIN_NOPULL);
    NRF_SAADC->RESOLUTION = SAADC_RESOLUTION_VAL_10bit;
    NRF_SAADC->OVERSAMPLE = SAADC_OVERSAMPLE_OVERSAMPLE_Bypass;
    NRF_SAADC->CH[0].CONFIG = (SAADC_CH_CONFIG_RESP_Bypass << SAADC_CH_CONFIG_RESP_Pos) |
                              (SAADC_CH_CONFIG_RESN_Bypass << SAADC_CH_CONFIG_RESN_Pos) |
                              (SAADC_CH_CONFIG_GAIN_Gain1_6 << SAADC_CH_CONFIG_GAIN_Pos) |
                              (SAADC_CH_CONFIG_REFSEL_Internal << SAADC_CH_CONFIG_REFSEL_Pos) |
                              (SAADC_CH_CONFIG_TACQ_40us << SAADC_CH_CONFIG_TACQ_Pos);
    NRF_SAADC->CH[0].PSELP = SAADC_CH_PSELP_PSELP_AnalogInput0 << SAADC_CH_PSELP_PSELP_Pos;
    NRF_SAADC->CH[0].PSELN = SAADC_CH_PSELN_PSELN_NC << SAADC_CH_PSELN_PSELN_Pos;
    NRF_SAADC->ENABLE = 1;
}

// Every wait here is bounded: a stuck SAADC must not hang the whole firmware
static bool wait_event(volatile uint32_t *event)
{
    for (uint32_t guard = 0; guard < 1000000; guard++)
    {
        if (*event)
        {
            *event = 0;
            return true;
        }
    }
    return false;
}

void battery_init(void)
{
    saadc_setup();
    NRF_SAADC->TASKS_CALIBRATEOFFSET = 1;
    wait_event(&NRF_SAADC->EVENTS_CALIBRATEDONE);
    NRF_SAADC->ENABLE = 0;
}

void battery_set_calibration(uint16_t per_mille)
{
    if (per_mille >= 800 && per_mille <= 1200)
        m_cal_per_mille = per_mille;
}

static int16_t sample_once(void)
{
    volatile int16_t value = 0;

    NRF_SAADC->RESULT.PTR = (uint32_t)&value;
    NRF_SAADC->RESULT.MAXCNT = 1;
    NRF_SAADC->EVENTS_STARTED = 0;
    NRF_SAADC->EVENTS_END = 0;
    NRF_SAADC->EVENTS_STOPPED = 0;

    NRF_SAADC->TASKS_START = 1;
    if (!wait_event(&NRF_SAADC->EVENTS_STARTED))
        return -1;

    NRF_SAADC->TASKS_SAMPLE = 1;
    if (!wait_event(&NRF_SAADC->EVENTS_END))
        return -1;

    NRF_SAADC->TASKS_STOP = 1;
    wait_event(&NRF_SAADC->EVENTS_STOPPED);

    return value;
}

static uint8_t soc_from_mv(uint16_t mv)
{
    if (mv >= soc_curve[0].mv)
        return 100;

    for (unsigned i = 1; i < SOC_POINTS; i++)
    {
        if (mv >= soc_curve[i].mv)
        {
            // Linear interpolation between the two bracketing points
            uint16_t span_mv = soc_curve[i - 1].mv - soc_curve[i].mv;
            uint8_t span_pct = soc_curve[i - 1].percent - soc_curve[i].percent;
            uint16_t above = mv - soc_curve[i].mv;
            return soc_curve[i].percent + (above * span_pct) / span_mv;
        }
    }
    return 0;
}

void battery_update(void)
{
    // Let the supply recover before sampling. This firmware keeps the receiver
    // on far more than the original did, so measuring straight after a sweep
    // reads the sag across the cell's internal resistance, not its charge.
    nrf_delay_ms(3);

    saadc_setup();

    // Throw the first conversion away, it is taken before the input has
    // settled. Of the rest keep the second highest: the high samples are the
    // ones taken furthest from a current spike, and dropping the very highest
    // guards against a single bad conversion.
    (void)sample_once();

    int32_t best = -1, second = -1;
    for (int i = 0; i < BAT_OVERSAMPLE; i++)
    {
        int16_t v = sample_once();
        if (v < 0)
            continue;
        if (v > best)
        {
            second = best;
            best = v;
        }
        else if (v > second)
        {
            second = v;
        }
    }
    NRF_SAADC->ENABLE = 0;

    int32_t adc = (second >= 0) ? second : best;
    if (adc < 0)
        return;

    g_battery.raw_adc = (int16_t)adc;

    // 10 bit, gain 1/6 and the 0.6V internal reference give a 3.6V full scale,
    // the divider on the board adds a factor of 1.451: mv ~= adc * 5.101
    uint32_t mv = ((uint32_t)adc * 20405u) / 4000u;
    mv = (mv * m_cal_per_mille) / 1000u;

    g_battery.charging = (nrf_gpio_pin_read(PIN_CHRG_STAT) == 0);
    g_battery.mv_raw = (uint16_t)mv;

    // A reading outside any plausible LiPo range means the measurement failed,
    // not that the battery is flat. Report it and change nothing else.
    if (mv < BAT_SANE_MIN_MV || mv > BAT_SANE_MAX_MV)
    {
        g_battery.valid = false;
        g_battery.low = false;
        g_battery.critical = false;
        return;
    }

    // Slow average so the reading does not jump around with the load. Seeded
    // on the first valid sample so the display is right immediately.
    if (m_filter_mv == 0)
        m_filter_mv = mv;
    else
        m_filter_mv = (m_filter_mv * 7 + mv) / 8;

    g_battery.mv = (uint16_t)m_filter_mv;
    g_battery.valid = true;

    // The percentage only moves when it really moved: a one point wobble
    // between two readings is noise, not discharge
    uint8_t pct = soc_from_mv(g_battery.mv);
    if (g_battery.percent == 0 || pct > g_battery.percent + 1 || pct + 1 < g_battery.percent)
        g_battery.percent = pct;

    // Two consecutive readings before believing the battery is low, so one
    // bad sample can never trigger anything
    bool low_now = !g_battery.charging && g_battery.mv < LOW_BATTERY_MV;
    bool crit_now = !g_battery.charging && g_battery.mv < CRITICAL_BATTERY_MV;

    g_battery.low = low_now && m_low_streak > 0;
    g_battery.critical = crit_now && m_low_streak >= 3;
    m_low_streak = low_now ? (uint8_t)(m_low_streak + 1) : 0;
}
