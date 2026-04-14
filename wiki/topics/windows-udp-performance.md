---
topic: Windows UDP Performance
last_updated: 2026-04-13
status: active
---

# Windows UDP Performance

## Summary

Receiving ST 2110-20 uncompressed video on Windows requires careful tuning of Winsock UDP sockets. The default Windows SO_RCVBUF is 8KB, which is catastrophically small for high-bitrate multicast. The library sets 128MB, which absorbs scheduling jitter at 1080p60. Jumbo frames reduce packet rates from ~166K pps to ~28K pps, bringing the Realtek RTL8127 10GbE NIC into comfortable territory.

## Socket Buffer Sizing

| Setting | Value | Notes |
|---------|-------|-------|
| Default SO_RCVBUF | 8,192 bytes | Loses packets immediately at any ST 2110 bitrate |
| Recommended | 128 MB (134,217,728 bytes) | Absorbs ~430ms of 1080p60 data |
| Maximum | ~2 GB | 64-bit Windows with sufficient RAM |

Always verify with `getsockopt()` after `setsockopt()`. Windows may silently clamp to a lower value.

## Receive Path Options

| Method | Batch? | Status in libst2110rx |
|--------|--------|-----------------------|
| `recvfrom()` | No | Phase 1 (current). One packet per syscall. Sufficient for 1080p60 with jumbo frames. |
| IOCP (Overlapped I/O) | Yes | Phase 4 (planned). Pre-post 256-1024 buffers. Kernel fills asynchronously. |
| RIO (Registered I/O) | Yes | Phase 4 (stretch). Pre-registered buffers, completion queues. Can handle 1M+ pps. |

## RTL8127 Specific Concerns

The Realtek RTL8127 is a PCIe Gen4 x1 10GbE NIC. It is the receive NIC on test-host.

- RSS limited to 4 queues (vs 16+ on Intel/Mellanox). Multicast often hashes to a single queue.
- Community reports of packet drops above ~100K pps.
- Interrupt coalescing configurable in Device Manager Advanced Properties ("Adaptive" recommended).
- No DPDK driver support on any platform.
- Jumbo frames essential to keep packet rate under 30K pps for 1080p60.

## Eliminated Approaches

- **Npcap:** Not kernel bypass (NDIS 6 LWF). No performance benefit over Winsock. Proprietary license incompatible with MIT open source.
- **DPDK on Windows:** Requires Windows Server 2025, NetUIO driver (test signing), Intel E810/E830 NICs only. Zero Realtek support.
- **Raw sockets (SOCK_RAW):** Requires admin. Minimal overhead savings. Adds complexity with no gain.

## IGMP Multicast Join

```c
struct ip_mreq mreq = {0};
mreq.imr_multiaddr.s_addr = inet_addr("239.1.0.1");
mreq.imr_interface.s_addr = inet_addr("10.0.0.X");  /* specific NIC IP */
setsockopt(sock, IPPROTO_IP, IP_ADD_MEMBERSHIP, (char *)&mreq, sizeof(mreq));
```

On multi-NIC systems, always bind to the specific interface IP. Using `INADDR_ANY` may join on the wrong adapter.

## Key Findings

- **1080p60 is at the boundary** of reliable OS socket reception per DELTACAST research. Jumbo frames + 128MB buffer + high-priority thread make it viable.
- **2160p60 is not feasible** with OS sockets. Would require kernel bypass (DPDK/VMA) which is not available for RTL8127.
- **2160p30 is the stretch goal.** ~57K pps with jumbo frames. May work with IOCP/RIO but unproven on RTL8127.

## Sources

- [[../../docs/plans/2026-04-13-obs-st2110-research.md]]
