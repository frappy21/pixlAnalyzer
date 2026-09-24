// Every sweep must visit every channel exactly once, whatever stride the
// randomiser picks. Getting this wrong silently leaves part of the band unmeasured.
#include <stdio.h>
#include <string.h>

#include "sweep_order.h"

int main(void)
{
    int failures = 0;
    uint8_t seen[256];

    for (uint8_t count = 1; count <= 141; count++)
    {
        uint16_t seed = 0xACE1;
        for (int round = 0; round < 64; round++)
        {
            seed = sweep_order_next_seed(seed);
            uint8_t stride = sweep_order_stride(count, seed);
            uint8_t idx = (uint8_t)(seed % count);

            memset(seen, 0, sizeof(seen));
            for (uint8_t n = 0; n < count; n++)
            {
                seen[idx]++;
                idx = (uint8_t)((idx + stride) % count);
            }

            for (uint8_t i = 0; i < count; i++)
            {
                if (seen[i] != 1)
                {
                    printf("  FAIL count=%u stride=%u: channel %u visited %u times\n",
                           count, stride, i, seen[i]);
                    failures++;
                    round = 64;
                    break;
                }
            }
        }
    }

    printf("  %s all channel counts 1..141 x 64 seeds cover the band exactly once\n",
           failures ? "FAIL" : "ok  ");
    printf("\n%s (%d failures)\n", failures ? "FAILED" : "ALL PASS", failures);
    return failures != 0;
}
