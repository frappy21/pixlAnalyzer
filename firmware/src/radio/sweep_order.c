#include "sweep_order.h"

static uint8_t gcd8(uint8_t a, uint8_t b)
{
    while (b)
    {
        uint8_t t = a % b;
        a = b;
        b = t;
    }
    return a;
}

uint16_t sweep_order_next_seed(uint16_t seed)
{
    return (uint16_t)(seed * 25173u + 13849u);
}

uint8_t sweep_order_stride(uint8_t count, uint16_t seed)
{
    if (count < 3)
        return 1;

    uint8_t stride = (uint8_t)(1 + ((seed >> 3) % (count - 1)));
    for (uint8_t tries = 0; tries < count; tries++)
    {
        if (gcd8(stride, count) == 1)
            return stride;
        stride = (uint8_t)(stride + 1);
        if (stride >= count)
            stride = 1;
    }
    return 1; // count is prime-free of options only if it is 1, keep it safe
}
