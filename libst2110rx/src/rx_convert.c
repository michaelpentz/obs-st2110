#include "rx_convert.h"

#include <stddef.h>

void rx_convert_be10_to_uyvy(
    const uint8_t *src,
    uint32_t src_stride,
    uint8_t *dst,
    uint32_t dst_stride,
    uint32_t width,
    uint32_t height)
{
    uint32_t y;
    uint32_t pgroups = width / 2;

    for (y = 0; y < height; ++y) {
        const uint8_t *sp = src + (size_t)y * src_stride;
        uint8_t *dp = dst + (size_t)y * dst_stride;
        uint32_t g;

        for (g = 0; g < pgroups; ++g) {
            uint16_t cb = (uint16_t)((sp[0] << 2) | (sp[1] >> 6));
            uint16_t y0 = (uint16_t)(((sp[1] & 0x3F) << 4) | (sp[2] >> 4));
            uint16_t cr = (uint16_t)(((sp[2] & 0x0F) << 6) | (sp[3] >> 2));
            uint16_t y1 = (uint16_t)(((sp[3] & 0x03) << 8) | sp[4]);

            dp[0] = (uint8_t)(cb >> 2);
            dp[1] = (uint8_t)(y0 >> 2);
            dp[2] = (uint8_t)(cr >> 2);
            dp[3] = (uint8_t)(y1 >> 2);

            sp += 5;
            dp += 4;
        }
    }
}
