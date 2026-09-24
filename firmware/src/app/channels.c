#include "channels.h"

#include "scanner.h"

// WiFi 1..13 sit at 2412 + 5*(n-1), each occupying about 20MHz
#define WIFI_COUNT 13
// 802.15.4 channels 11..26 sit at 2405 + 5*(n-11), 2MHz wide
#define ZIGBEE_COUNT 16

static const uint16_t ble_adv[3] = {2402, 2426, 2480};

const char *channels_plan_name(uint8_t plan)
{
    switch (plan)
    {
    case PLAN_WIFI:
        return "WIFI";
    case PLAN_BLE:
        return "BLE";
    case PLAN_ZIGBEE:
        return "802.15.4";
    default:
        return "OFF";
    }
}

uint8_t channels_plan_count(uint8_t plan)
{
    switch (plan)
    {
    case PLAN_WIFI:
        return WIFI_COUNT;
    case PLAN_BLE:
        return 3;
    case PLAN_ZIGBEE:
        return ZIGBEE_COUNT;
    default:
        return 0;
    }
}

bool channels_plan_get(uint8_t plan, uint8_t index, chan_mark_t *out)
{
    if (!out || index >= channels_plan_count(plan))
        return false;

    switch (plan)
    {
    case PLAN_WIFI:
        out->center_mhz = 2412 + 5 * index;
        out->half_width = 10;
        out->number = index + 1;
        return true;

    case PLAN_BLE:
        out->center_mhz = ble_adv[index];
        out->half_width = 1;
        out->number = 37 + index;
        return true;

    case PLAN_ZIGBEE:
        out->center_mhz = 2405 + 5 * index;
        out->half_width = 1;
        out->number = 11 + index;
        return true;

    default:
        return false;
    }
}

const char *channels_label(uint16_t mhz)
{
    static char buf[5];

    // BLE advertising channels are the most specific, check them first
    for (int i = 0; i < 3; i++)
    {
        if (mhz == ble_adv[i])
        {
            buf[0] = 'B';
            buf[1] = '3';
            buf[2] = (char)('7' + i);
            buf[3] = '\0';
            return buf;
        }
    }

    if (mhz >= 2405 && mhz <= 2480 && ((mhz - 2405) % 5) == 0)
    {
        int n = 11 + (mhz - 2405) / 5;
        buf[0] = 'Z';
        buf[1] = (char)('0' + n / 10);
        buf[2] = (char)('0' + n % 10);
        buf[3] = '\0';
        return buf;
    }

    if (mhz >= 2412 && mhz <= 2472 && ((mhz - 2412) % 5) == 0)
    {
        int n = 1 + (mhz - 2412) / 5;
        buf[0] = 'W';
        if (n < 10)
        {
            buf[1] = (char)('0' + n);
            buf[2] = '\0';
        }
        else
        {
            buf[1] = (char)('0' + n / 10);
            buf[2] = (char)('0' + n % 10);
            buf[3] = '\0';
        }
        return buf;
    }

    return "";
}

uint8_t channels_occupancy(uint8_t plan, uint8_t index)
{
    chan_mark_t mark;
    if (!channels_plan_get(plan, index, &mark))
        return 0;

    uint16_t lo = mark.center_mhz - mark.half_width;
    uint16_t hi = mark.center_mhz + mark.half_width;

    uint32_t sum = 0;
    uint16_t n = 0;
    for (uint8_t i = 0; i < scanner_count(); i++)
    {
        uint16_t f = scanner_mhz(i);
        if (f >= lo && f <= hi)
        {
            sum += g_scan[i].busy;
            n++;
        }
    }
    return n ? (uint8_t)(sum / n) : 0;
}

uint8_t channels_best_wifi(uint8_t *score_out)
{
    static const uint8_t candidates[3] = {1, 6, 11};
    uint8_t best = 1;
    uint16_t best_busy = 0xFFFF;

    for (int i = 0; i < 3; i++)
    {
        uint8_t busy = channels_occupancy(PLAN_WIFI, candidates[i] - 1);
        if (busy < best_busy)
        {
            best_busy = busy;
            best = candidates[i];
        }
    }

    if (score_out)
        *score_out = (uint8_t)(255 - (best_busy > 255 ? 255 : best_busy));
    return best;
}
