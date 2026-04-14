#include "rx_platform.h"
#include "st2110rx.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#ifndef INET_ADDRSTRLEN
#define INET_ADDRSTRLEN 16
#endif

typedef struct {
    rx_socket_t sock;
    struct ip_mreq mreq;
    int used;
} rx_membership_t;

static rx_membership_t g_memberships[16];

static void rx_log(st2110rx_log_cb cb, void *ud, st2110rx_log_level_t level, const char *fmt, ...)
{
    char msg[256];
    va_list args;

    if (!cb) {
        return;
    }

    va_start(args, fmt);
    (void)vsnprintf(msg, sizeof(msg), fmt, args);
    va_end(args);
    cb(level, msg, ud);
}

static int rx_register_membership(rx_socket_t sock, const struct ip_mreq *mreq)
{
    size_t i;
    for (i = 0; i < sizeof(g_memberships) / sizeof(g_memberships[0]); ++i) {
        if (!g_memberships[i].used) {
            g_memberships[i].used = 1;
            g_memberships[i].sock = sock;
            g_memberships[i].mreq = *mreq;
            return 0;
        }
    }
    return -1;
}

static int rx_find_membership(rx_socket_t sock)
{
    size_t i;
    for (i = 0; i < sizeof(g_memberships) / sizeof(g_memberships[0]); ++i) {
        if (g_memberships[i].used && g_memberships[i].sock == sock) {
            return (int)i;
        }
    }
    return -1;
}

rx_socket_t rx_udp_create(const st2110rx_config_t *config, st2110rx_log_cb log_cb, void *log_ud)
{
    rx_socket_t sock;
    struct sockaddr_in bind_addr;
    struct ip_mreq mreq;
    int reuse = 1;
    int buf_size;
    int actual = 0;
    int optlen = (int)sizeof(actual);
    unsigned long mcast_ip;
    unsigned long iface_ip;

    if (!config || !config->multicast_addr) {
        return RX_INVALID_SOCKET;
    }

    sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == RX_INVALID_SOCKET) {
        rx_log(log_cb, log_ud, ST2110RX_LOG_ERROR, "socket() failed: %d", rx_socket_error());
        return RX_INVALID_SOCKET;
    }

    (void)setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, (const char *)&reuse, (int)sizeof(reuse));

    buf_size = config->socket_buffer_size ? (int)config->socket_buffer_size : ST2110RX_DEFAULT_SOCKET_BUF;
    (void)setsockopt(sock, SOL_SOCKET, SO_RCVBUF, (const char *)&buf_size, (int)sizeof(buf_size));
    if (getsockopt(sock, SOL_SOCKET, SO_RCVBUF, (char *)&actual, &optlen) == 0 && actual < buf_size) {
        rx_log(log_cb,
               log_ud,
               ST2110RX_LOG_WARN,
               "SO_RCVBUF requested=%d actual=%d (clamped by OS)",
               buf_size,
               actual);
    }

    memset(&bind_addr, 0, sizeof(bind_addr));
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_port = htons(config->port ? config->port : ST2110RX_DEFAULT_PORT);
    bind_addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(sock, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) != 0) {
        rx_log(log_cb, log_ud, ST2110RX_LOG_ERROR, "bind() failed: %d", rx_socket_error());
        rx_socket_close(sock);
        return RX_INVALID_SOCKET;
    }

    mcast_ip = inet_addr(config->multicast_addr);
    if (mcast_ip == INADDR_NONE) {
        rx_log(log_cb, log_ud, ST2110RX_LOG_ERROR, "invalid multicast address: %s", config->multicast_addr);
        rx_socket_close(sock);
        return RX_INVALID_SOCKET;
    }

    iface_ip = (config->interface_addr && config->interface_addr[0] != '\0')
        ? inet_addr(config->interface_addr)
        : htonl(INADDR_ANY);
    if (iface_ip == INADDR_NONE && config->interface_addr && config->interface_addr[0] != '\0') {
        rx_log(log_cb, log_ud, ST2110RX_LOG_ERROR, "invalid interface address: %s", config->interface_addr);
        rx_socket_close(sock);
        return RX_INVALID_SOCKET;
    }

    memset(&mreq, 0, sizeof(mreq));
    mreq.imr_multiaddr.s_addr = mcast_ip;
    mreq.imr_interface.s_addr = iface_ip;
    if (setsockopt(sock, IPPROTO_IP, IP_ADD_MEMBERSHIP, (const char *)&mreq, (int)sizeof(mreq)) != 0) {
        rx_log(log_cb, log_ud, ST2110RX_LOG_ERROR, "IP_ADD_MEMBERSHIP failed: %d", rx_socket_error());
        rx_socket_close(sock);
        return RX_INVALID_SOCKET;
    }

    if (rx_register_membership(sock, &mreq) != 0) {
        rx_log(log_cb, log_ud, ST2110RX_LOG_WARN, "membership registry full, drop on close may be skipped");
    }

    return sock;
}

void rx_udp_destroy(rx_socket_t sock)
{
    int idx;

    if (sock == RX_INVALID_SOCKET) {
        return;
    }

    idx = rx_find_membership(sock);
    if (idx >= 0) {
        (void)setsockopt(sock,
                         IPPROTO_IP,
                         IP_DROP_MEMBERSHIP,
                         (const char *)&g_memberships[idx].mreq,
                         (int)sizeof(g_memberships[idx].mreq));
        g_memberships[idx].used = 0;
    }

    (void)rx_socket_close(sock);
}
