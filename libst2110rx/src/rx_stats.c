#include "rx_stats.h"

#include <string.h>

#ifdef _WIN32
#include <windows.h>
static uint64_t get_time_us(void)
{
    LARGE_INTEGER freq;
    LARGE_INTEGER count;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&count);
    return (uint64_t)(count.QuadPart * 1000000ULL / (uint64_t)freq.QuadPart);
}
#else
#include <time.h>
static uint64_t get_time_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
}
#endif

void rx_stats_init(rx_stats_state_t *s)
{
    memset(s, 0, sizeof(*s));
}

void rx_stats_packet(rx_stats_state_t *s, uint32_t rtp_timestamp)
{
    uint64_t now_us = get_time_us();

    rx_atomic64_add(&s->packets_received, 1);

    if (s->has_prev) {
        int64_t d_arrival = (int64_t)(now_us - s->last_arrival_us);
        int64_t d_rtp = (int64_t)((uint32_t)(rtp_timestamp - s->last_rtp_ts)) * 1000000LL / 90000LL;
        int64_t diff = d_arrival - d_rtp;
        if (diff < 0) {
            diff = -diff;
        }
        /* Seqlock: odd sequence = write in progress */
        rx_atomic64_add(&s->seq, 1);
        s->jitter += ((double)diff - s->jitter) / 16.0;
        s->avg_interpacket_gap_us += ((double)d_arrival - s->avg_interpacket_gap_us) / 256.0;
        rx_atomic64_add(&s->seq, 1);
    }

    s->last_rtp_ts = rtp_timestamp;
    s->last_arrival_us = now_us;
    s->has_prev = true;
}

void rx_stats_frame(rx_stats_state_t *s, bool incomplete)
{
    rx_atomic64_add(&s->frames_received, 1);
    if (incomplete) {
        rx_atomic64_add(&s->frames_dropped, 1);
    }
}

void rx_stats_lost(rx_stats_state_t *s, uint64_t count)
{
    rx_atomic64_add(&s->packets_lost, (int64_t)count);
}

void rx_stats_snapshot(const rx_stats_state_t *s, st2110rx_stats_t *out)
{
    int64_t seq1, seq2;
    int attempts = 0;

    if (!s || !out) {
        return;
    }

    out->frames_received = (uint64_t)rx_atomic64_load(&s->frames_received);
    out->frames_dropped = (uint64_t)rx_atomic64_load(&s->frames_dropped);
    out->packets_received = (uint64_t)rx_atomic64_load(&s->packets_received);
    out->packets_lost = (uint64_t)rx_atomic64_load(&s->packets_lost);

    /* Seqlock read: retry until we get a consistent pair */
    do {
        seq1 = rx_atomic64_load(&s->seq);
        out->jitter_us = s->jitter;
        out->avg_interpacket_gap_us = s->avg_interpacket_gap_us;
        seq2 = rx_atomic64_load(&s->seq);
        if (++attempts > 1000) {
            break; /* give up, return potentially torn values rather than spinning forever */
        }
    } while (seq1 != seq2 || (seq1 & 1));
}
