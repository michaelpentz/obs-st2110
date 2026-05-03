#ifndef RX_RFC4175_H
#define RX_RFC4175_H

#include "st2110rx.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint16_t flags;
    uint16_t seq;
    uint32_t timestamp;
    uint32_t ssrc;
} rx_rtp_header_t;

typedef struct {
    uint16_t length;
    uint16_t line_no;
    uint16_t offset;
} rx_rfc4175_line_header_t;

typedef struct {
    uint8_t *fill_buf;
    uint8_t *deliver_buf;
    uint32_t fill_stride;
    uint32_t width;
    uint32_t height;
    uint32_t pgroup_size;
    uint32_t pixels_per_group;
    uint16_t last_seq;
    uint32_t last_ts;
    bool has_prev_packet;
    bool frame_incomplete;
    bool frame_delivered;
    uint32_t frame_number;
    uint32_t delivered_incomplete;
    uint32_t delivered_timestamp;
} rx_frame_assembler_t;

int rx_assembler_init(rx_frame_assembler_t *a, uint32_t width, uint32_t height,
                      st2110rx_pixfmt_t input_fmt);
void rx_assembler_free(rx_frame_assembler_t *a);
int rx_assembler_process_packet(
    rx_frame_assembler_t *a,
    const uint8_t *pkt,
    size_t pkt_len,
    st2110rx_frame_cb frame_cb,
    void *user_data,
    st2110rx_pixfmt_t output_fmt,
    uint64_t *packets_lost);

#endif
