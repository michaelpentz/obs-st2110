#include "st2110rx.h"

#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
static void sleep_ms(unsigned ms)
{
    Sleep(ms);
}
#else
#include <unistd.h>
static void sleep_ms(unsigned ms)
{
    usleep(ms * 1000);
}
#endif

typedef struct {
    st2110rx_t *rx;
    FILE *output;
    uint32_t count_limit;
    uint32_t stats_every;
    volatile uint32_t frames_seen;
    volatile sig_atomic_t stop_requested;
} cli_state_t;

static cli_state_t g_cli;

static void print_usage(const char *prog)
{
    printf("Usage: %s [options]\n", prog);
    printf("  --addr <multicast addr>    (default: 239.1.0.1)\n");
    printf("  --port <port>              (default: 20000)\n");
    printf("  --iface <interface ip>     (default: 0.0.0.0)\n");
    printf("  --width <pixels>           (default: 1920)\n");
    printf("  --height <pixels>          (default: 1080)\n");
    printf("  --count <frames>           (default: 0 = infinite)\n");
    printf("  --output <file>            (default: none)\n");
    printf("  --stats <N>                print stats every N frames\n");
    printf("  --help                     show this help\n");
}

static int parse_u32(const char *s, uint32_t *out)
{
    char *end = NULL;
    unsigned long v;
    if (!s || !out) {
        return -1;
    }
    v = strtoul(s, &end, 10);
    if (end == s || (end && *end != '\0') || v > 0xFFFFFFFFUL) {
        return -1;
    }
    *out = (uint32_t)v;
    return 0;
}

static void signal_handler(int sig)
{
    (void)sig;
    g_cli.stop_requested = 1;
}

static void frame_cb(const st2110rx_frame_t *frame, void *user_data)
{
    cli_state_t *state = (cli_state_t *)user_data;
    uint32_t count;

    if (!state || !frame) {
        return;
    }

    count = state->frames_seen + 1;
    state->frames_seen = count;

    printf("Frame %u: %ux%u bytes=%u incomplete=%d\n",
           frame->frame_number,
           frame->width,
           frame->height,
           frame->linesize[0] * frame->height,
           frame->incomplete ? 1 : 0);

    if (state->output) {
        size_t frame_bytes = (size_t)frame->linesize[0] * frame->height;
        (void)fwrite(frame->data[0], 1, frame_bytes, state->output);
        (void)fflush(state->output);
    }

    if (state->stats_every > 0 && (count % state->stats_every) == 0 && state->rx) {
        st2110rx_stats_t stats;
        if (st2110rx_get_stats(state->rx, &stats) == ST2110RX_OK) {
            printf("Stats: frames=%llu dropped=%llu packets=%llu lost=%llu jitter_us=%.2f\n",
                   (unsigned long long)stats.frames_received,
                   (unsigned long long)stats.frames_dropped,
                   (unsigned long long)stats.packets_received,
                   (unsigned long long)stats.packets_lost,
                   stats.jitter_us);
        }
    }

    if (state->count_limit > 0 && count >= state->count_limit) {
        state->stop_requested = 1;
    }
}

int main(int argc, char *argv[])
{
    const char *addr = "239.1.0.1";
    const char *iface = "0.0.0.0";
    const char *output_path = NULL;
    uint32_t port = ST2110RX_DEFAULT_PORT;
    uint32_t width = 1920;
    uint32_t height = 1080;
    uint32_t count = 0;
    uint32_t stats_every = 0;
    int i;
    st2110rx_config_t config;
    st2110rx_t *rx = NULL;

    memset(&g_cli, 0, sizeof(g_cli));

    for (i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        } else if (strcmp(argv[i], "--addr") == 0 && i + 1 < argc) {
            addr = argv[++i];
        } else if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            if (parse_u32(argv[++i], &port) != 0 || port > 65535) {
                fprintf(stderr, "Invalid --port value\n");
                return 1;
            }
        } else if (strcmp(argv[i], "--iface") == 0 && i + 1 < argc) {
            iface = argv[++i];
        } else if (strcmp(argv[i], "--width") == 0 && i + 1 < argc) {
            if (parse_u32(argv[++i], &width) != 0 || width == 0) {
                fprintf(stderr, "Invalid --width value\n");
                return 1;
            }
        } else if (strcmp(argv[i], "--height") == 0 && i + 1 < argc) {
            if (parse_u32(argv[++i], &height) != 0 || height == 0) {
                fprintf(stderr, "Invalid --height value\n");
                return 1;
            }
        } else if (strcmp(argv[i], "--count") == 0 && i + 1 < argc) {
            if (parse_u32(argv[++i], &count) != 0) {
                fprintf(stderr, "Invalid --count value\n");
                return 1;
            }
        } else if (strcmp(argv[i], "--output") == 0 && i + 1 < argc) {
            output_path = argv[++i];
        } else if (strcmp(argv[i], "--stats") == 0 && i + 1 < argc) {
            if (parse_u32(argv[++i], &stats_every) != 0) {
                fprintf(stderr, "Invalid --stats value\n");
                return 1;
            }
        } else {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            print_usage(argv[0]);
            return 1;
        }
    }

    if (output_path) {
        g_cli.output = fopen(output_path, "wb");
        if (!g_cli.output) {
            fprintf(stderr, "Failed to open output file: %s\n", output_path);
            return 1;
        }
    }

    g_cli.count_limit = count;
    g_cli.stats_every = stats_every;

    config = (st2110rx_config_t){
        .multicast_addr = addr,
        .port = (uint16_t)port,
        .interface_addr = iface,
        .width = width,
        .height = height,
        .input_fmt = ST2110RX_FMT_YCBCR422_10BIT,
        .output_fmt = ST2110RX_FMT_UYVY,
        .frame_cb = frame_cb,
        .user_data = &g_cli,
    };

    rx = st2110rx_create(&config);
    if (!rx) {
        if (g_cli.output) {
            fclose(g_cli.output);
        }
        fprintf(stderr, "Failed to create receiver\n");
        return 1;
    }
    g_cli.rx = rx;

    signal(SIGINT, signal_handler);

    if (st2110rx_start(rx) != ST2110RX_OK) {
        st2110rx_destroy(rx);
        if (g_cli.output) {
            fclose(g_cli.output);
        }
        fprintf(stderr, "Failed to start receiver\n");
        return 1;
    }

    while (!g_cli.stop_requested) {
        sleep_ms(100);
    }

    st2110rx_stop(rx);

    {
        st2110rx_stats_t stats;
        if (st2110rx_get_stats(rx, &stats) == ST2110RX_OK) {
            printf("Final stats: frames=%llu dropped=%llu packets=%llu lost=%llu gap_us=%.2f jitter_us=%.2f\n",
                   (unsigned long long)stats.frames_received,
                   (unsigned long long)stats.frames_dropped,
                   (unsigned long long)stats.packets_received,
                   (unsigned long long)stats.packets_lost,
                   stats.avg_interpacket_gap_us,
                   stats.jitter_us);
        }
    }

    st2110rx_destroy(rx);
    if (g_cli.output) {
        fclose(g_cli.output);
    }
    return 0;
}
