#ifndef RX_STATS_H
#define RX_STATS_H

#include "rx_platform.h"
#include "st2110rx.h"

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    rx_atomic64_t frames_received;
    rx_atomic64_t frames_dropped;
    rx_atomic64_t packets_received;
    rx_atomic64_t packets_lost;
    double jitter;
    double avg_interpacket_gap_us;
    uint32_t last_rtp_ts;
    uint64_t last_arrival_us;
    bool has_prev;
} rx_stats_state_t;

void rx_stats_init(rx_stats_state_t *s);
void rx_stats_packet(rx_stats_state_t *s, uint32_t rtp_timestamp);
void rx_stats_frame(rx_stats_state_t *s, bool incomplete);
void rx_stats_lost(rx_stats_state_t *s, uint64_t count);
void rx_stats_snapshot(const rx_stats_state_t *s, st2110rx_stats_t *out);

#endif
