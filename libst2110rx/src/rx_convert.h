#ifndef RX_CONVERT_H
#define RX_CONVERT_H

#include <stdint.h>

void rx_convert_be10_to_uyvy(
    const uint8_t *src,
    uint32_t src_stride,
    uint8_t *dst,
    uint32_t dst_stride,
    uint32_t width,
    uint32_t height);

#endif
