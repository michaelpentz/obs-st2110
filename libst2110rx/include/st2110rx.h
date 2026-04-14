#ifndef ST2110RX_H
#define ST2110RX_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct st2110rx st2110rx_t;

typedef enum {
    ST2110RX_FMT_YCBCR422_10BIT = 0,
    ST2110RX_FMT_UYVY,
    ST2110RX_FMT_P010,
    ST2110RX_FMT_V210,
} st2110rx_pixfmt_t;

typedef struct {
    uint8_t *data[4];
    uint32_t linesize[4];
    uint32_t width;
    uint32_t height;
    st2110rx_pixfmt_t format;
    uint64_t timestamp_ns;
    uint32_t frame_number;
    bool incomplete;
} st2110rx_frame_t;

typedef void (*st2110rx_frame_cb)(const st2110rx_frame_t *frame, void *user_data);

typedef enum {
    ST2110RX_LOG_ERROR = 0,
    ST2110RX_LOG_WARN,
    ST2110RX_LOG_INFO,
    ST2110RX_LOG_DEBUG,
} st2110rx_log_level_t;

typedef void (*st2110rx_log_cb)(st2110rx_log_level_t level, const char *msg, void *user_data);

typedef struct {
    const char *multicast_addr;
    uint16_t port;
    const char *interface_addr;
    uint32_t width;
    uint32_t height;
    st2110rx_pixfmt_t input_fmt;
    st2110rx_pixfmt_t output_fmt;
    uint32_t socket_buffer_size;
    bool enable_jumbo;
    st2110rx_frame_cb frame_cb;
    void *user_data;
    st2110rx_log_cb log_cb;
    void *log_user_data;
} st2110rx_config_t;

typedef struct {
    uint64_t frames_received;
    uint64_t frames_dropped;
    uint64_t packets_received;
    uint64_t packets_lost;
    double avg_interpacket_gap_us;
    double jitter_us;
} st2110rx_stats_t;

#define ST2110RX_OK 0
#define ST2110RX_ERR_SOCKET -1
#define ST2110RX_ERR_BIND -2
#define ST2110RX_ERR_IGMP -3
#define ST2110RX_ERR_THREAD -4
#define ST2110RX_ERR_INVALID -5
#define ST2110RX_ERR_ALLOC -6

#define ST2110RX_DEFAULT_SOCKET_BUF (128 * 1024 * 1024)
#define ST2110RX_DEFAULT_PORT 20000

st2110rx_t *st2110rx_create(const st2110rx_config_t *config);
int st2110rx_start(st2110rx_t *rx);
void st2110rx_stop(st2110rx_t *rx);
void st2110rx_destroy(st2110rx_t *rx);
int st2110rx_get_stats(st2110rx_t *rx, st2110rx_stats_t *stats);

#ifdef __cplusplus
}
#endif

#endif
