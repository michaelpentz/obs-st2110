#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
typedef SOCKET tx_socket_t;
static void sleep_ms(unsigned ms) { Sleep(ms); }
static uint64_t now_ms(void) { return GetTickCount64(); }
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
typedef int tx_socket_t;
#define INVALID_SOCKET (-1)
#define SOCKET_ERROR (-1)
static void sleep_ms(unsigned ms) { usleep(ms * 1000); }
static uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;
}
#endif

typedef struct {
    const char *addr;
    uint16_t port;
    const char *iface;
    uint32_t width;
    uint32_t height;
    uint32_t fps;
    uint32_t count;
    uint32_t mtu;
    uint32_t drop_pct;
    int burst;
} tx_config_t;

static void usage(const char *prog)
{
    printf("Usage: %s [options]\n", prog);
    printf("  --addr <multicast addr> (default: 239.1.0.1)\n");
    printf("  --port <port>           (default: 20000)\n");
    printf("  --iface <ip>            (default: 0.0.0.0)\n");
    printf("  --width <pixels>        (default: 1920)\n");
    printf("  --height <pixels>       (default: 1080)\n");
    printf("  --fps <frames/sec>      (default: 30)\n");
    printf("  --count <frames>        (default: 0=infinite)\n");
    printf("  --mtu <1500|9000>       (default: 1500)\n");
    printf("  --drop-pct <0-100>      (default: 0)\n");
    printf("  --burst                 (disable frame pacing)\n");
    printf("  --help                  show this help\n");
}

static int parse_u32(const char *s, uint32_t *out)
{
    char *end = NULL;
    unsigned long v = strtoul(s, &end, 10);
    if (!s || !out || end == s || *end != '\0' || v > 0xFFFFFFFFUL) {
        return -1;
    }
    *out = (uint32_t)v;
    return 0;
}

static int parse_args(int argc, char **argv, tx_config_t *cfg)
{
    int i;
    *cfg = (tx_config_t){
        .addr = "239.1.0.1",
        .port = 20000,
        .iface = "0.0.0.0",
        .width = 1920,
        .height = 1080,
        .fps = 30,
        .count = 0,
        .mtu = 1500,
        .drop_pct = 0,
        .burst = 0,
    };

    for (i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--help") == 0) {
            usage(argv[0]);
            return 1;
        } else if (strcmp(argv[i], "--addr") == 0 && i + 1 < argc) {
            cfg->addr = argv[++i];
        } else if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            uint32_t v;
            if (parse_u32(argv[++i], &v) != 0 || v > 65535) {
                return -1;
            }
            cfg->port = (uint16_t)v;
        } else if (strcmp(argv[i], "--iface") == 0 && i + 1 < argc) {
            cfg->iface = argv[++i];
        } else if (strcmp(argv[i], "--width") == 0 && i + 1 < argc) {
            if (parse_u32(argv[++i], &cfg->width) != 0 || cfg->width == 0 || (cfg->width % 2) != 0) {
                return -1;
            }
        } else if (strcmp(argv[i], "--height") == 0 && i + 1 < argc) {
            if (parse_u32(argv[++i], &cfg->height) != 0 || cfg->height == 0) {
                return -1;
            }
        } else if (strcmp(argv[i], "--fps") == 0 && i + 1 < argc) {
            if (parse_u32(argv[++i], &cfg->fps) != 0 || cfg->fps == 0) {
                return -1;
            }
        } else if (strcmp(argv[i], "--count") == 0 && i + 1 < argc) {
            if (parse_u32(argv[++i], &cfg->count) != 0) {
                return -1;
            }
        } else if (strcmp(argv[i], "--mtu") == 0 && i + 1 < argc) {
            if (parse_u32(argv[++i], &cfg->mtu) != 0 || (cfg->mtu != 1500 && cfg->mtu != 9000)) {
                return -1;
            }
        } else if (strcmp(argv[i], "--drop-pct") == 0 && i + 1 < argc) {
            if (parse_u32(argv[++i], &cfg->drop_pct) != 0 || cfg->drop_pct > 100) {
                return -1;
            }
        } else if (strcmp(argv[i], "--burst") == 0) {
            cfg->burst = 1;
        } else {
            return -1;
        }
    }

    return 0;
}

static void pack_group(uint8_t *dst, uint16_t cb, uint16_t y0, uint16_t cr, uint16_t y1)
{
    dst[0] = (uint8_t)(cb >> 2);
    dst[1] = (uint8_t)(((cb & 0x3U) << 6) | (y0 >> 4));
    dst[2] = (uint8_t)(((y0 & 0xFU) << 4) | (cr >> 6));
    dst[3] = (uint8_t)(((cr & 0x3FU) << 2) | (y1 >> 8));
    dst[4] = (uint8_t)(y1 & 0xFFU);
}

static void generate_colorbars_be10(uint8_t *frame, uint32_t width, uint32_t height, uint32_t stride)
{
    static const uint16_t bars[8][3] = {
        {512, 940, 512},
        {64, 877, 553},
        {671, 754, 159},
        {223, 691, 200},
        {800, 566, 824},
        {352, 503, 865},
        {960, 380, 471},
        {512, 64, 512},
    };
    uint32_t y;
    uint32_t groups = width / 2;
    uint32_t groups_per_bar = groups / 8;
    if (groups_per_bar == 0) {
        groups_per_bar = 1;
    }

    for (y = 0; y < height; ++y) {
        uint8_t *row = frame + (size_t)y * stride;
        uint32_t g;
        for (g = 0; g < groups; ++g) {
            uint32_t bar = g / groups_per_bar;
            if (bar > 7) {
                bar = 7;
            }
            pack_group(row + (size_t)g * 5,
                       bars[bar][0],
                       bars[bar][1],
                       bars[bar][2],
                       bars[bar][1]);
        }
    }
}

static void write_rtp_header(uint8_t *pkt, uint16_t seq, uint32_t ts, uint32_t ssrc, int marker)
{
    pkt[0] = 0x80;
    pkt[1] = (uint8_t)(96 | (marker ? 0x80 : 0x00));
    pkt[2] = (uint8_t)(seq >> 8);
    pkt[3] = (uint8_t)(seq & 0xFF);
    pkt[4] = (uint8_t)(ts >> 24);
    pkt[5] = (uint8_t)((ts >> 16) & 0xFF);
    pkt[6] = (uint8_t)((ts >> 8) & 0xFF);
    pkt[7] = (uint8_t)(ts & 0xFF);
    pkt[8] = (uint8_t)(ssrc >> 24);
    pkt[9] = (uint8_t)((ssrc >> 16) & 0xFF);
    pkt[10] = (uint8_t)((ssrc >> 8) & 0xFF);
    pkt[11] = (uint8_t)(ssrc & 0xFF);
}

int main(int argc, char **argv)
{
    tx_config_t cfg;
    tx_socket_t sock = INVALID_SOCKET;
    struct sockaddr_in dst;
    uint8_t *frame = NULL;
    uint8_t *packet = NULL;
    uint32_t stride;
    uint32_t frame_index = 0;
    uint32_t seq = 1;
    uint32_t ts = 0;
    uint32_t ssrc = 0x11223344U;
    uint32_t ts_step;
    size_t max_udp_payload;
    size_t max_segment;
#ifdef _WIN32
    WSADATA wsa;
#endif

    int parse = parse_args(argc, argv, &cfg);
    if (parse == 1) {
        return 0;
    }
    if (parse != 0) {
        usage(argv[0]);
        return 1;
    }

    ts_step = 90000U / cfg.fps;
    stride = (cfg.width / 2U) * 5U;
    frame = (uint8_t *)malloc((size_t)stride * cfg.height);
    if (!frame) {
        fprintf(stderr, "failed to allocate frame buffer\n");
        return 1;
    }
    generate_colorbars_be10(frame, cfg.width, cfg.height, stride);

    if (cfg.mtu <= 28) {
        fprintf(stderr, "invalid mtu\n");
        free(frame);
        return 1;
    }
    max_udp_payload = (size_t)cfg.mtu - 28U;
    packet = (uint8_t *)malloc(max_udp_payload);
    if (!packet) {
        fprintf(stderr, "failed to allocate packet buffer\n");
        free(frame);
        return 1;
    }

    max_segment = max_udp_payload - 12U - 6U;
    max_segment = (max_segment / 5U) * 5U;
    if (max_segment == 0) {
        fprintf(stderr, "mtu too small for RFC4175 payload\n");
        free(packet);
        free(frame);
        return 1;
    }

#ifdef _WIN32
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        fprintf(stderr, "WSAStartup failed\n");
        free(packet);
        free(frame);
        return 1;
    }
#endif

    sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == INVALID_SOCKET) {
        fprintf(stderr, "socket() failed\n");
        goto fail;
    }

    memset(&dst, 0, sizeof(dst));
    dst.sin_family = AF_INET;
    dst.sin_port = htons(cfg.port);
    dst.sin_addr.s_addr = inet_addr(cfg.addr);
    if (dst.sin_addr.s_addr == INADDR_NONE) {
        fprintf(stderr, "invalid multicast address: %s\n", cfg.addr);
        goto fail;
    }

    if (cfg.iface && cfg.iface[0] != '\0' && strcmp(cfg.iface, "0.0.0.0") != 0) {
        struct in_addr iface_addr;
        iface_addr.s_addr = inet_addr(cfg.iface);
        if (iface_addr.s_addr == INADDR_NONE) {
            fprintf(stderr, "invalid interface address: %s\n", cfg.iface);
            goto fail;
        }
        if (setsockopt(sock, IPPROTO_IP, IP_MULTICAST_IF, (const char *)&iface_addr, sizeof(iface_addr)) != 0) {
            fprintf(stderr, "IP_MULTICAST_IF failed\n");
            goto fail;
        }
    }

    srand((unsigned)time(NULL));
    printf("sending to %s:%u %ux%u fps=%u count=%u mtu=%u drop=%u%%\n",
           cfg.addr,
           (unsigned)cfg.port,
           (unsigned)cfg.width,
           (unsigned)cfg.height,
           (unsigned)cfg.fps,
           (unsigned)cfg.count,
           (unsigned)cfg.mtu,
           (unsigned)cfg.drop_pct);

    while (cfg.count == 0 || frame_index < cfg.count) {
        uint64_t frame_start_ms = now_ms();
        uint32_t line;

        for (line = 0; line < cfg.height; ++line) {
            size_t line_offset = (size_t)line * stride;
            size_t remaining = stride;
            uint16_t pixel_offset = 0;

            while (remaining > 0) {
                size_t seg_len = remaining > max_segment ? max_segment : remaining;
                int continuation = (remaining > seg_len) ? 1 : 0;
                int is_last_packet = ((line + 1U) == cfg.height) && !continuation;
                size_t pkt_len = 12 + 6 + seg_len;

                write_rtp_header(packet, (uint16_t)seq++, ts, ssrc, is_last_packet);

                packet[12] = (uint8_t)(seg_len >> 8);
                packet[13] = (uint8_t)(seg_len & 0xFF);
                packet[14] = (uint8_t)(line >> 8);
                packet[15] = (uint8_t)(line & 0xFF);
                packet[16] = (uint8_t)((continuation ? 0x80 : 0x00) | (pixel_offset >> 8));
                packet[17] = (uint8_t)(pixel_offset & 0xFF);
                memcpy(packet + 18, frame + line_offset + (stride - remaining), seg_len);

                if (cfg.drop_pct == 0 || (rand() % 100) >= (int)cfg.drop_pct) {
                    if (sendto(sock,
                               (const char *)packet,
                               (int)pkt_len,
                               0,
                               (struct sockaddr *)&dst,
                               (int)sizeof(dst)) == SOCKET_ERROR) {
                        fprintf(stderr, "sendto() failed\n");
                        goto fail;
                    }
                }

                remaining -= seg_len;
                pixel_offset = (uint16_t)(pixel_offset + (seg_len / 5U) * 2U);
            }
        }

        printf("frame %u sent (rtp ts=%u)\n", frame_index, ts);
        frame_index++;
        ts += ts_step;

        if (!cfg.burst) {
            uint64_t elapsed_ms = now_ms() - frame_start_ms;
            uint64_t target_ms = 1000ULL / cfg.fps;
            if (elapsed_ms < target_ms) {
                sleep_ms((unsigned)(target_ms - elapsed_ms));
            }
        }
    }

    printf("done\n");

    free(packet);
    free(frame);
#ifdef _WIN32
    closesocket(sock);
    WSACleanup();
#else
    close(sock);
#endif
    return 0;

fail:
    if (packet) {
        free(packet);
    }
    if (frame) {
        free(frame);
    }
    if (sock != INVALID_SOCKET) {
#ifdef _WIN32
        closesocket(sock);
        WSACleanup();
#else
        close(sock);
#endif
    } else {
#ifdef _WIN32
        WSACleanup();
#endif
    }
    return 1;
}
