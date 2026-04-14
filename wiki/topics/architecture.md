---
topic: Architecture
last_updated: 2026-04-13
status: active
---

# Architecture

## Summary

obs-st2110 is a monorepo containing two components: libst2110rx (a portable C99 static library for receiving SMPTE ST 2110-20 uncompressed video over RTP multicast) and an OBS Studio source plugin that wraps it. The library has zero external dependencies and targets Windows (MSVC) and Linux (GCC/Clang).

The receiver pipeline is: UDP socket (Winsock/POSIX) receives RTP packets, RFC 4175 depayloader extracts scan line segments into a double-buffered frame assembler, a color converter transforms wire format (YCbCr 4:2:2 10-bit big-endian packed) to an output format (UYVY 8-bit for Phase 1, P010/V210 10-bit for Phase 3), and a callback delivers the completed frame to the caller.

One receiver thread per instance. The callback fires from that thread. Callers must not block in the callback. OBS `obs_source_output_video()` copies the frame internally, making the OBS plugin callback trivial.

## Components

| Component | Type | Purpose |
|-----------|------|---------|
| `libst2110rx/include/st2110rx.h` | Public API | Config, frame callback, stats, lifecycle |
| `libst2110rx/src/rx_platform.h` | Internal | Platform abstraction (Winsock/POSIX, threads, atomics) |
| `libst2110rx/src/rx_socket.c` | Internal | UDP multicast socket with IGMP join, 128MB SO_RCVBUF |
| `libst2110rx/src/rx_rfc4175.c` | Internal | RFC 4175 depayloader and double-buffered frame assembler |
| `libst2110rx/src/rx_convert.c` | Internal | Scalar color conversion (BE10 to UYVY) |
| `libst2110rx/src/rx_stats.c` | Internal | Lock-free stats with RFC 3550 jitter tracking |
| `libst2110rx/src/rx_core.c` | Internal | Lifecycle, receiver thread, wiring |
| `libst2110rx/tools/st2110rx-cli.c` | Tool | CLI receiver for testing without OBS |
| `libst2110rx/tools/st2110tx-test.c` | Tool | Synthetic RFC 4175 stream generator |
| `obs-plugin/src/obs-st2110.c` | Plugin | OBS async video source (Phase 2) |

## Key Design Decisions

- **Static library, not shared.** The OBS plugin links libst2110rx statically, producing a single .dll with no runtime dependencies.
- **Double-buffered frame assembly.** Assembler writes to `fill_buf` while `deliver_buf` is being consumed by the callback. Swap on frame completion. No mutex needed since the receiver thread owns both operations sequentially.
- **Incomplete frames delivered with flag.** Packet loss sets `frame.incomplete = true`. The caller decides whether to display or drop. Silently dropping frames would hide network problems.
- **Socket receive behind function pointer.** Phase 1 uses `recvfrom()`. Phase 4 upgrades to IOCP without changing the assembler or converter code.
- **Color conversion in separate translation unit.** AVX2 fast path (Phase 4) compiles with `/arch:AVX2` flag. Runtime CPUID dispatch via function pointer.

## Phased Delivery

| Phase | Deliverable | Status |
|-------|-------------|--------|
| 0 | obs-gstreamer hardware validation | Pending (needs Blackmagic device) |
| 1 | libst2110rx MVP (recvfrom, RFC 4175, UYVY) | Complete (2026-04-13) |
| 2 | OBS source plugin MVP | Planned |
| 3 | 10-bit output (P010, V210) | Planned |
| 4 | IOCP receive, AVX2 conversion | Planned |
| 5 | SDP import, stats UI, polish | Planned |

## Sources

- [[../../docs/plans/2026-04-13-obs-st2110-design.md]]
- [[../../docs/plans/2026-04-13-obs-st2110-plan.md]]
