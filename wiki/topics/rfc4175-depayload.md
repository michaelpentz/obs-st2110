---
topic: RFC 4175 Depayload
last_updated: 2026-04-13
status: active
---

# RFC 4175 Depayload

## Summary

RFC 4175 defines how uncompressed video is packetized into RTP for transport over IP networks. ST 2110-20 uses this format for all uncompressed video essences. The depayloader extracts scan line segments from RTP packets and assembles them into complete video frames.

Each RTP packet contains a 12-byte RTP header followed by one or more 6-byte RFC 4175 scan line headers, each followed by its payload data. A continuation bit chains multiple line segments within a single packet.

## Wire Format

**RTP Header (12 bytes):**
- Sequence number (16-bit): gap detection for packet loss
- Timestamp (32-bit, 90kHz clock): identifies which frame this packet belongs to
- Marker bit: set on the last packet of a frame

**RFC 4175 Scan Line Header (6 bytes per segment):**
- `length` (16-bit): payload bytes for this segment
- `line_no` (15-bit) + `field` (1-bit): scan line number and interlace field
- `offset` (15-bit) + `continuation` (1-bit): horizontal pixel offset and continuation flag

**Payload:** Raw pixel data for the scan line segment, in wire format (YCbCr 4:2:2 10-bit big-endian packed for ST 2110-20).

## 10-bit Pixel Group Format

YCbCr 4:2:2 10-bit packs 2 pixels into 5 bytes (big-endian):

```
Byte 0:         Cb[9:2]
Byte 1: [7:6]   Cb[1:0]    [5:0]   Y0[9:4]
Byte 2: [7:4]   Y0[3:0]    [3:0]   Cr[9:6]
Byte 3: [7:2]   Cr[5:0]    [1:0]   Y1[9:8]
Byte 4:         Y1[7:0]
```

Extraction is pure arithmetic (bit shifts and masks, ~4 operations per sample). No byte-swap needed because the shifts handle big-endian ordering implicitly.

## Frame Assembly Algorithm

1. Parse RTP header. Extract sequence number, timestamp, marker bit.
2. Detect sequence gaps (packet loss). Mark current frame incomplete.
3. If timestamp changed without marker bit, the previous frame was incomplete. Deliver it and start a new frame.
4. Loop over 6-byte scan line headers (continuation bit chains them).
5. For each segment: `memcpy` payload into frame buffer at `(line * stride) + (pixel_offset / pixels_per_group) * pgroup_bytes`.
6. On marker bit: frame complete. Run color converter, deliver via callback, swap double buffers.

This algorithm is reimplemented from the RFC 4175 specification, using FFmpeg's `rtpdec_rfc4175.c` as an algorithmic reference. The core depayload logic is approximately 80 lines of C.

## Packet Rate at Different MTUs

| Format | Bitrate | Standard MTU (1376B payload) | Jumbo (8000B payload) |
|--------|---------|------------------------------|------------------------|
| 1080p60 | ~2.49 Gbps | ~166,000 pps | ~28,500 pps |
| 2160p30 | ~4.98 Gbps | ~332,000 pps | ~57,100 pps |

Jumbo frames are critical for keeping packet rates within the RTL8127's comfortable operating range (~30K pps).

## Loopback Test Results (2026-04-13)

Tested with synthetic stream generator (`st2110tx-test`) sending to `st2110rx-cli` via multicast on the LAN interface.

- 320x240, 3 frames, 1500 MTU
- 3/3 frames received, 0 packets lost, 480 total packets
- Jitter: ~83us (local multicast)
- One frame marked incomplete due to timestamp-change detection edge case (cosmetic, not data loss)

## Key References

- RFC 4175: RTP Payload Format for Uncompressed Video
- FFmpeg `rtpdec_rfc4175.c` (~80 LOC core algorithm)
- FFmpeg `bitpacked_dec.c` (~20 LOC 10-bit decoder)
- SMPTE ST 2110-20: Professional Media Over Managed IP Networks

## Sources

- [[../../docs/plans/2026-04-13-obs-st2110-research.md]]
