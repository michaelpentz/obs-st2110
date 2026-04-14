#include "rx_rfc4175.h"

#include <stdlib.h>
#include <string.h>

#define PGROUP_422_10BIT 5
#define PIXELS_PER_GROUP 2
#define RTP_MARKER_MASK 0x0080
#define RTP_X_MASK 0x1000

int rx_assembler_init(rx_frame_assembler_t *a, uint32_t width, uint32_t height)
{
    size_t buf_size;

    if (!a || width == 0 || height == 0 || (width % 2) != 0) {
        return -1;
    }

    memset(a, 0, sizeof(*a));
    a->width = width;
    a->height = height;
    a->pgroup_size = PGROUP_422_10BIT;
    a->pixels_per_group = PIXELS_PER_GROUP;
    a->fill_stride = (width / PIXELS_PER_GROUP) * PGROUP_422_10BIT;

    buf_size = (size_t)a->fill_stride * height;
    a->fill_buf = (uint8_t *)calloc(1, buf_size);
    a->deliver_buf = (uint8_t *)calloc(1, buf_size);
    if (!a->fill_buf || !a->deliver_buf) {
        free(a->fill_buf);
        free(a->deliver_buf);
        a->fill_buf = NULL;
        a->deliver_buf = NULL;
        return -1;
    }

    return 0;
}

void rx_assembler_free(rx_frame_assembler_t *a)
{
    if (!a) {
        return;
    }
    free(a->fill_buf);
    free(a->deliver_buf);
    a->fill_buf = NULL;
    a->deliver_buf = NULL;
}

static const uint8_t *parse_rtp_header(
    const uint8_t *pkt,
    size_t len,
    uint16_t *seq,
    uint32_t *timestamp,
    bool *marker)
{
    uint16_t flags;
    uint8_t cc;
    size_t hdr_len;

    if (!pkt || len < 12 || !seq || !timestamp || !marker) {
        return NULL;
    }

    flags = (uint16_t)((pkt[0] << 8) | pkt[1]);
    *marker = (flags & RTP_MARKER_MASK) != 0;
    *seq = (uint16_t)((pkt[2] << 8) | pkt[3]);
    *timestamp = ((uint32_t)pkt[4] << 24) | ((uint32_t)pkt[5] << 16) | ((uint32_t)pkt[6] << 8) | (uint32_t)pkt[7];

    cc = pkt[0] & 0x0F;
    hdr_len = 12 + (size_t)cc * 4;

    if (flags & RTP_X_MASK) {
        uint16_t ext_len_words;
        if (len < hdr_len + 4) {
            return NULL;
        }
        ext_len_words = (uint16_t)((pkt[hdr_len + 2] << 8) | pkt[hdr_len + 3]);
        hdr_len += 4 + (size_t)ext_len_words * 4;
    }

    if (len < hdr_len) {
        return NULL;
    }

    return pkt + hdr_len;
}

static void deliver_frame(
    rx_frame_assembler_t *a,
    st2110rx_frame_cb frame_cb,
    void *user_data,
    st2110rx_pixfmt_t output_fmt,
    uint32_t timestamp)
{
    size_t buf_size;
    uint8_t *tmp;
    st2110rx_frame_t frame;

    tmp = a->deliver_buf;
    a->deliver_buf = a->fill_buf;
    a->fill_buf = tmp;

    memset(&frame, 0, sizeof(frame));
    frame.data[0] = a->deliver_buf;
    frame.linesize[0] = a->fill_stride;
    frame.width = a->width;
    frame.height = a->height;
    frame.format = output_fmt;
    frame.frame_number = a->frame_number++;
    frame.incomplete = a->frame_incomplete;
    frame.timestamp_ns = ((uint64_t)timestamp * 1000000000ULL) / 90000ULL;
    frame_cb(&frame, user_data);

    a->delivered_incomplete = a->frame_incomplete ? 1U : 0U;
    a->delivered_timestamp = timestamp;

    buf_size = (size_t)a->fill_stride * a->height;
    memset(a->fill_buf, 0, buf_size);
    a->frame_incomplete = false;
}

int rx_assembler_process_packet(
    rx_frame_assembler_t *a,
    const uint8_t *pkt,
    size_t pkt_len,
    st2110rx_frame_cb frame_cb,
    void *user_data,
    st2110rx_pixfmt_t output_fmt,
    uint64_t *packets_lost)
{
    const uint8_t *payload;
    size_t payload_len;
    const uint8_t *p;
    const uint8_t *end;
    uint16_t seq;
    uint32_t timestamp;
    bool marker;
    int frames_completed = 0;

    if (!a || !pkt || !frame_cb) {
        return 0;
    }

    payload = parse_rtp_header(pkt, pkt_len, &seq, &timestamp, &marker);
    if (!payload) {
        return 0;
    }

    payload_len = pkt_len - (size_t)(payload - pkt);

    if (a->has_prev_packet) {
        uint16_t expected = (uint16_t)(a->last_seq + 1);
        if (seq != expected) {
            uint16_t gap = (uint16_t)(seq - expected);
            if (packets_lost) {
                *packets_lost += gap;
            }
            a->frame_incomplete = true;
        }
    }

    if (a->has_prev_packet && timestamp != a->last_ts && !marker) {
        a->frame_incomplete = true;
        deliver_frame(a, frame_cb, user_data, output_fmt, a->last_ts);
        frames_completed++;
    }

    a->last_seq = seq;
    a->last_ts = timestamp;
    a->has_prev_packet = true;

    p = payload;
    end = payload + payload_len;
    while (p + 6 <= end) {
        uint16_t seg_length = (uint16_t)((p[0] << 8) | p[1]);
        uint16_t line_field = (uint16_t)((p[2] << 8) | p[3]);
        uint16_t offset_cont = (uint16_t)((p[4] << 8) | p[5]);
        uint16_t line_no = (uint16_t)(line_field & 0x7FFF);
        uint16_t pixel_offset = (uint16_t)(offset_cont & 0x7FFF);
        bool continuation = (offset_cont & 0x8000) != 0;
        size_t byte_offset;
        size_t dst_offset;
        size_t buf_size;

        p += 6;
        if (line_no >= a->height) {
            break;
        }
        if (p + seg_length > end) {
            break;
        }

        byte_offset = ((size_t)pixel_offset / a->pixels_per_group) * a->pgroup_size;
        dst_offset = (size_t)line_no * a->fill_stride + byte_offset;
        buf_size = (size_t)a->fill_stride * a->height;
        if (dst_offset + seg_length > buf_size) {
            break;
        }

        memcpy(a->fill_buf + dst_offset, p, seg_length);
        p += seg_length;

        if (!continuation) {
            break;
        }
    }

    if (marker) {
        deliver_frame(a, frame_cb, user_data, output_fmt, timestamp);
        frames_completed++;
    }

    return frames_completed;
}
