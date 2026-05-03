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

/* Number of overlapped receives kept posted on Windows IOCP. Sized to
   absorb roughly 1 ms of bursty traffic at 250 kpps without dropping
   on the kernel side. Each buffer is jumbo-frame sized (~9200 B), so
   the pool footprint is ~600 KB. */
#define RX_IOCP_NUM_BUFFERS 64
#define RX_IOCP_BUFFER_SIZE 9200

#ifdef _WIN32
typedef struct {
    OVERLAPPED ov;          /* MUST be first: we cast OVERLAPPED* back to this */
    WSABUF wsabuf;
    uint8_t buf[RX_IOCP_BUFFER_SIZE];
    SOCKADDR_STORAGE from;
    INT from_len;
} rx_iocp_buffer_t;
#endif

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

    uint8_t *pkt_buf;            /* used by non-IOCP (Linux) recvfrom path */
    size_t pkt_buf_size;

    uint8_t *convert_buf;
    uint32_t convert_stride;

#ifdef _WIN32
    HANDLE iocp;
    rx_iocp_buffer_t *iocp_buffers;
#endif
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
        if (frame->format == ST2110RX_FMT_UYVY) {
            /* Wire format already 8-bit UYVY-packed (pgroup=4). The
               assembler buffer IS the UYVY byte layout; no conversion
               needed. Just pass the pointer through. */
            out.data[0] = frame->data[0];
            out.linesize[0] = frame->linesize[0];
            out.format = ST2110RX_FMT_UYVY;
        } else if (frame->format == ST2110RX_FMT_YCBCR422_10BIT) {
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
            /* Unsupported input -> UYVY combination. Skip frame rather
               than deliver garbage. */
            return;
        }
    } else {
        out.data[0] = frame->data[0];
        out.linesize[0] = frame->linesize[0];
        out.format = frame->format;
    }

    rx->config.frame_cb(&out, rx->config.user_data);
}

static void rx_process_packet(st2110rx_t *rx, const uint8_t *pkt, size_t n)
{
    uint64_t lost = 0;
    int completed;

    rx_stats_packet(&rx->stats, rx_extract_rtp_timestamp(pkt, n));

    completed = rx_assembler_process_packet(
        &rx->assembler,
        pkt,
        n,
        rx_internal_frame_cb,
        rx,
        rx->config.input_fmt,
        &lost);

    if (lost) {
        rx_stats_lost(&rx->stats, lost);
    }
    if (completed > 0) {
        rx_stats_frame(&rx->stats, rx->assembler.delivered_incomplete != 0);
    }
}

#ifdef _WIN32
/* Post one overlapped WSARecvFrom against the buffer. The completion lands
   on the IOCP. WSA_IO_PENDING is the expected return for asynchronous
   completion — anything else is a real error. */
static int rx_iocp_post(st2110rx_t *rx, rx_iocp_buffer_t *buf)
{
    DWORD bytes = 0;
    DWORD flags = 0;
    int rc;

    memset(&buf->ov, 0, sizeof(buf->ov));
    buf->wsabuf.buf = (CHAR *)buf->buf;
    buf->wsabuf.len = (ULONG)sizeof(buf->buf);
    buf->from_len = (INT)sizeof(buf->from);

    rc = WSARecvFrom(rx->sock, &buf->wsabuf, 1, &bytes, &flags,
                     (struct sockaddr *)&buf->from, &buf->from_len,
                     &buf->ov, NULL);
    if (rc == 0) {
        return 0; /* completed inline; will still queue a completion packet */
    }
    if (WSAGetLastError() == WSA_IO_PENDING) {
        return 0;
    }
    return -1;
}

/* GetQueuedCompletionStatusEx dequeues up to N completions in one syscall.
   At 225 kpps for 1080p60, draining one packet per syscall costs ~225k
   GetQueuedCompletionStatus calls per second; Windows syscall overhead
   alone (a few µs each) caps single-threaded receive throughput well
   below line rate. Batching ~64 completions per syscall drops overhead
   by an order of magnitude and lets us keep up. */
#define RX_IOCP_BATCH 64

static void rx_receiver_loop(st2110rx_t *rx)
{
    int i;
    OVERLAPPED_ENTRY entries[RX_IOCP_BATCH];
    ULONG removed;
    rx_iocp_buffer_t *buf;
    BOOL ok;
    ULONG j;

    rx_thread_set_high_priority();

    /* Prime the pump: post every buffer once. Kernel fans out incoming
       datagrams across them and queues completions on the IOCP. */
    for (i = 0; i < RX_IOCP_NUM_BUFFERS; ++i) {
        if (rx_iocp_post(rx, &rx->iocp_buffers[i]) != 0) {
            rx_log(rx, ST2110RX_LOG_ERROR, "WSARecvFrom prime failed: %d", WSAGetLastError());
            return;
        }
    }

    while (rx_atomic64_load(&rx->running)) {
        removed = 0;
        ok = GetQueuedCompletionStatusEx(rx->iocp,
                                         entries,
                                         RX_IOCP_BATCH,
                                         &removed,
                                         INFINITE,
                                         FALSE);

        if (!rx_atomic64_load(&rx->running)) {
            break;
        }
        if (!ok || removed == 0) {
            continue;
        }

        for (j = 0; j < removed; ++j) {
            if (!entries[j].lpOverlapped) {
                continue; /* shutdown wakeup */
            }
            buf = (rx_iocp_buffer_t *)entries[j].lpOverlapped;

            if (entries[j].dwNumberOfBytesTransferred > 0) {
                rx_process_packet(rx,
                                  buf->buf,
                                  (size_t)entries[j].dwNumberOfBytesTransferred);
            }

            if (rx_atomic64_load(&rx->running)) {
                (void)rx_iocp_post(rx, buf);
            }
        }
    }
}
#else
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
        rx_process_packet(rx, rx->pkt_buf, (size_t)n);
    }
}
#endif

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

    if (rx_assembler_init(&rx->assembler, width, height, config->input_fmt) != 0) {
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

#ifdef _WIN32
    /* Allocate the overlapped buffer pool and bind the socket to an IOCP.
       Without this the receive thread would block in WSARecvFrom one packet
       at a time, which can't keep up with 1080p60 raw video on Windows
       (~225 kpps). With IOCP we keep RX_IOCP_NUM_BUFFERS posted at all
       times, letting the kernel batch deliveries. */
    rx->iocp_buffers = (rx_iocp_buffer_t *)calloc(RX_IOCP_NUM_BUFFERS, sizeof(*rx->iocp_buffers));
    if (!rx->iocp_buffers) {
        rx_log(rx, ST2110RX_LOG_ERROR, "iocp buffer pool alloc failed");
        rx_udp_destroy(rx->sock, &rx->mreq);
        rx->sock = RX_INVALID_SOCKET;
        rx_socket_cleanup();
        rx->sockets_initialized = false;
        return ST2110RX_ERR_ALLOC;
    }

    rx->iocp = CreateIoCompletionPort((HANDLE)rx->sock, NULL, 0, 1);
    if (!rx->iocp) {
        rx_log(rx, ST2110RX_LOG_ERROR, "CreateIoCompletionPort failed: %lu",
               (unsigned long)GetLastError());
        free(rx->iocp_buffers);
        rx->iocp_buffers = NULL;
        rx_udp_destroy(rx->sock, &rx->mreq);
        rx->sock = RX_INVALID_SOCKET;
        rx_socket_cleanup();
        rx->sockets_initialized = false;
        return ST2110RX_ERR_SOCKET;
    }
#endif

    rx_atomic64_store(&rx->running, 1);
    if (rx_thread_create(&rx->thread, rx_thread_entry, rx) != 0) {
        rx_atomic64_store(&rx->running, 0);
#ifdef _WIN32
        if (rx->iocp) {
            CloseHandle(rx->iocp);
            rx->iocp = NULL;
        }
        free(rx->iocp_buffers);
        rx->iocp_buffers = NULL;
#endif
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

#ifdef _WIN32
    /* Wake the receive thread out of GetQueuedCompletionStatus. */
    if (rx->iocp) {
        PostQueuedCompletionStatus(rx->iocp, 0, 0, NULL);
    }
#endif

    if (rx->sock != RX_INVALID_SOCKET) {
        /* Closing the socket also cancels any outstanding overlapped IOs,
           which fires error completions that the receive loop ignores
           because running is already 0. */
        rx_udp_destroy(rx->sock, &rx->mreq);
        rx->sock = RX_INVALID_SOCKET;
    }

    if (rx->thread_started) {
        rx_thread_join(rx->thread);
        rx->thread_started = false;
    }

#ifdef _WIN32
    if (rx->iocp) {
        CloseHandle(rx->iocp);
        rx->iocp = NULL;
    }
    free(rx->iocp_buffers);
    rx->iocp_buffers = NULL;
#endif

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
