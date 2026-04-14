#include "rx_convert.h"
#include "rx_platform.h"
#include "rx_rfc4175.h"
#include "rx_stats.h"
#include "st2110rx.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

rx_socket_t rx_udp_create(const st2110rx_config_t *config, st2110rx_log_cb log_cb, void *log_ud,
                           struct ip_mreq *mreq_out);
void rx_udp_destroy(rx_socket_t sock, const struct ip_mreq *mreq);

struct st2110rx {
    st2110rx_config_t config;
    rx_socket_t sock;
    struct ip_mreq mreq;       /* per-instance IGMP membership (no global state) */
    rx_thread_t thread;
    rx_atomic64_t running;      /* 1 = running, 0 = stopped (atomic for thread safety) */
    bool thread_started;
    bool sockets_initialized;

    rx_frame_assembler_t assembler;
    rx_stats_state_t stats;

    uint8_t *pkt_buf;
    size_t pkt_buf_size;

    uint8_t *convert_buf;
    uint32_t convert_stride;
};

static void rx_log(st2110rx_t *rx, st2110rx_log_level_t level, const char *fmt, ...)
{
    char msg[256];
    va_list args;

    if (!rx || !rx->config.log_cb) {
        return;
    }

    va_start(args, fmt);
    (void)vsnprintf(msg, sizeof(msg), fmt, args);
    va_end(args);
    rx->config.log_cb(level, msg, rx->config.log_user_data);
}

static uint32_t rx_extract_rtp_timestamp(const uint8_t *pkt, size_t len)
{
    if (!pkt || len < 8) {
        return 0;
    }
    return ((uint32_t)pkt[4] << 24) | ((uint32_t)pkt[5] << 16) | ((uint32_t)pkt[6] << 8) | (uint32_t)pkt[7];
}

static void rx_internal_frame_cb(const st2110rx_frame_t *frame, void *user_data)
{
    st2110rx_t *rx = (st2110rx_t *)user_data;
    st2110rx_frame_t out;

    if (!rx || !frame || !rx->config.frame_cb) {
        return;
    }

    memset(&out, 0, sizeof(out));
    out.width = frame->width;
    out.height = frame->height;
    out.timestamp_ns = frame->timestamp_ns;
    out.frame_number = frame->frame_number;
    out.incomplete = frame->incomplete;

    if (rx->config.output_fmt == ST2110RX_FMT_UYVY) {
        rx_convert_be10_to_uyvy(
            frame->data[0],
            frame->linesize[0],
            rx->convert_buf,
            rx->convert_stride,
            frame->width,
            frame->height);
        out.data[0] = rx->convert_buf;
        out.linesize[0] = rx->convert_stride;
        out.format = ST2110RX_FMT_UYVY;
    } else {
        out.data[0] = frame->data[0];
        out.linesize[0] = frame->linesize[0];
        out.format = ST2110RX_FMT_YCBCR422_10BIT;
    }

    rx->config.frame_cb(&out, rx->config.user_data);
}

static void rx_receiver_loop(st2110rx_t *rx)
{
    rx_thread_set_high_priority();

    while (rx_atomic64_load(&rx->running)) {
        int n = recvfrom(rx->sock, (char *)rx->pkt_buf, (int)rx->pkt_buf_size, 0, NULL, NULL);
        if (n <= 0) {
            if (!rx_atomic64_load(&rx->running)) {
                break;
            }
            continue;
        }

        rx_stats_packet(&rx->stats, rx_extract_rtp_timestamp(rx->pkt_buf, (size_t)n));

        {
            uint64_t lost = 0;
            int completed = rx_assembler_process_packet(
                &rx->assembler,
                rx->pkt_buf,
                (size_t)n,
                rx_internal_frame_cb,
                rx,
                ST2110RX_FMT_YCBCR422_10BIT,
                &lost);

            if (lost) {
                rx_stats_lost(&rx->stats, lost);
            }
            if (completed > 0) {
                rx_stats_frame(&rx->stats, rx->assembler.delivered_incomplete != 0);
            }
        }
    }
}

#ifdef _WIN32
static unsigned __stdcall rx_thread_entry(void *arg)
{
    rx_receiver_loop((st2110rx_t *)arg);
    return 0;
}
#else
static void *rx_thread_entry(void *arg)
{
    rx_receiver_loop((st2110rx_t *)arg);
    return NULL;
}
#endif

st2110rx_t *st2110rx_create(const st2110rx_config_t *config)
{
    st2110rx_t *rx;
    uint32_t width;
    uint32_t height;
    size_t convert_size;

    if (!config || !config->multicast_addr || !config->frame_cb || config->width == 0 || config->height == 0) {
        return NULL;
    }

    width = config->width;
    height = config->height;
    if ((width % 2) != 0) {
        return NULL;
    }

    rx = (st2110rx_t *)calloc(1, sizeof(*rx));
    if (!rx) {
        return NULL;
    }

    rx->config = *config;
    rx->sock = RX_INVALID_SOCKET;
    rx->pkt_buf_size = config->enable_jumbo ? 9200 : 1500;
    rx->pkt_buf = (uint8_t *)malloc(rx->pkt_buf_size);
    if (!rx->pkt_buf) {
        free(rx);
        return NULL;
    }

    rx->convert_stride = width * 2U;
    convert_size = (size_t)rx->convert_stride * height;
    rx->convert_buf = (uint8_t *)malloc(convert_size);
    if (!rx->convert_buf) {
        free(rx->pkt_buf);
        free(rx);
        return NULL;
    }

    if (rx_assembler_init(&rx->assembler, width, height) != 0) {
        free(rx->convert_buf);
        free(rx->pkt_buf);
        free(rx);
        return NULL;
    }

    rx_stats_init(&rx->stats);
    return rx;
}

int st2110rx_start(st2110rx_t *rx)
{
    if (!rx) {
        return ST2110RX_ERR_INVALID;
    }
    if (rx_atomic64_load(&rx->running)) {
        return ST2110RX_OK;
    }

    if (rx_socket_init() != 0) {
        rx_log(rx, ST2110RX_LOG_ERROR, "WSAStartup/socket init failed");
        return ST2110RX_ERR_SOCKET;
    }
    rx->sockets_initialized = true;

    rx->sock = rx_udp_create(&rx->config, rx->config.log_cb, rx->config.log_user_data, &rx->mreq);
    if (rx->sock == RX_INVALID_SOCKET) {
        if (rx->sockets_initialized) {
            rx_socket_cleanup();
            rx->sockets_initialized = false;
        }
        return ST2110RX_ERR_BIND;
    }

    rx_atomic64_store(&rx->running, 1);
    if (rx_thread_create(&rx->thread, rx_thread_entry, rx) != 0) {
        rx_atomic64_store(&rx->running, 0);
        rx_udp_destroy(rx->sock, &rx->mreq);
        rx->sock = RX_INVALID_SOCKET;
        if (rx->sockets_initialized) {
            rx_socket_cleanup();
            rx->sockets_initialized = false;
        }
        return ST2110RX_ERR_THREAD;
    }
    rx->thread_started = true;

    return ST2110RX_OK;
}

void st2110rx_stop(st2110rx_t *rx)
{
    if (!rx) {
        return;
    }

    rx_atomic64_store(&rx->running, 0);

    if (rx->sock != RX_INVALID_SOCKET) {
        rx_udp_destroy(rx->sock, &rx->mreq);
        rx->sock = RX_INVALID_SOCKET;
    }

    if (rx->thread_started) {
        rx_thread_join(rx->thread);
        rx->thread_started = false;
    }

    if (rx->sockets_initialized) {
        rx_socket_cleanup();
        rx->sockets_initialized = false;
    }
}

void st2110rx_destroy(st2110rx_t *rx)
{
    if (!rx) {
        return;
    }

    st2110rx_stop(rx);
    rx_assembler_free(&rx->assembler);
    free(rx->convert_buf);
    free(rx->pkt_buf);
    free(rx);
}

int st2110rx_get_stats(st2110rx_t *rx, st2110rx_stats_t *stats)
{
    if (!rx || !stats) {
        return ST2110RX_ERR_INVALID;
    }
    rx_stats_snapshot(&rx->stats, stats);
    return ST2110RX_OK;
}
