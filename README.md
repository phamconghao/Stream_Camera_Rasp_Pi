# Stream_Camera_Rasp_Pi

A C++ pipeline for streaming CSI camera video from a **Raspberry Pi 4B** over the network, using **libcamera** for capture, the **BCM2835 V4L2 M2M hardware encoder** for H.264, and **RTP/UDP** for delivery.

```
Camera (libcamera) → Capture → Raw Frame Pool/Queue → Encoder Thread (BCM2835 HW H.264)
    → Encoded Frame Pool/Queue → RTP Packetizer Thread (H.264 NAL parser) → RTP Packet Pool/Queue
    → UDP Sender Thread (fans out to every PLAYING RTSP session) → Network → RTSP/RTP Client(s)
```

Every stage hands off a **pointer**, never a copy — frame/packet bytes are copied exactly once, when read out of the camera or the hardware encoder. The data pipeline above is **lazily started** by the RTSP server on the first client `PLAY` and stopped again once the last client leaves — see `PipelineController` below.

> Branch: `csi_camera_streaming`

## Status

| Stage | Status |
|---|---|
| CSI camera capture (libcamera) | ✅ Implemented |
| Raw/Encoded frame pools & queues | ✅ Implemented |
| BCM2835 hardware H.264 encoder (`/dev/video11`) | ✅ Implemented |
| BCM2835 hardware H.264 decoder (`/dev/video10`, receiver side) | ✅ Implemented |
| H.264 NAL parser | ✅ Implemented |
| RTP packetizer thread (FU-A fragmentation, RFC 6184) | ✅ Implemented |
| Jitter buffer + RTP depacketizer (receiver side) | ✅ Implemented |
| UDP sender thread (multi-destination fan-out per RTSP session) | ✅ Implemented |
| RTCP Sender/Receiver Reports (RFC 3550, rtcp-mux) | ✅ Implemented |
| Adaptive bitrate (3-tier, loss-driven) | ✅ Implemented |
| Monitoring dashboard (`dashboard.html`) | ✅ Implemented |
| Dashcam-style circular H.264 recording (receiver side) | ✅ Implemented |
| RTSP server (`OPTIONS`/`DESCRIBE`/`SETUP`/`PLAY`/`TEARDOWN`, session registry, lazy pipeline start, real SDP from cached SPS/PPS) | ✅ Implemented (6/6 steps) — see `roadmap.md` |
| WebRTC (browser playback via ICE/DTLS/SRTP) | ⏳ Planned, not started — see `roadmap.md`'s Phase 22 sub-phase breakdown (22.1-22.6) |
| Security (SRTP for RTSP/RTP, RTSP auth, control channel auth) | ⏳ Not started |

Full phase-by-phase status, including implementation evidence per step, lives in [`roadmap.md`](./roadmap.md) — treat that file as the single source of truth over this README for anything more granular than the table above.

Each pipeline stage owns an independent `static std::atomic<bool>` running flag (e.g. `g_encoder_running`, `g_capture_running`), separate from the app-wide `app_state::g_running` flag that only `main()` writes to. This keeps shutdown clean and lets `PipelineController` start/stop individual threads per RTSP session without racing each other.

## Requirements

- Raspberry Pi 4B (BCM2835/BCM2711 V4L2 M2M H.264 encoder at `/dev/video11`)
- A CSI camera module supported by `libcamera`
- CMake ≥ 3.16
- GCC with C++17 support
- `libcamera` development package (`libcamera-dev`) discoverable via `pkg-config`
- OpenSSL development package (`libssl-dev`) discoverable via `pkg-config`
- libSRTP 2 development package (`libsrtp2-dev`) discoverable via `pkg-config`
- `pthread`

On Raspberry Pi OS/Debian, install the build dependencies with:

```bash
sudo apt update
sudo apt install -y build-essential cmake pkg-config libcamera-dev libssl-dev libsrtp2-dev
```

## Build

```bash
./build.sh
```

This does a clean CMake configure + build (`-j$(nproc)`) and produces the `camera_app` executable under `build/`.

Equivalent manual steps:

```bash
cmake -B build
cmake --build build -j$(nproc)
```

The build enables `-Wall -Wextra -Wpedantic -Werror`, so any new warning fails the build.

## Run

```bash
./build/camera_app [control_port] [rtsp_port]
```

- `control_port` — UDP port this sender listens on for keyframe-request/bitrate-feedback datagrams from a receiver (default: `5005`)
- `rtsp_port` — TCP port the RTSP server listens on (default: `8554`)

Example:

```bash
./build/camera_app 5005 8554
```

The app initializes hardware (camera, encoder) and pools/queues up front, then **starts the RTSP server and waits** — it does not stream to anyone until a client actually sends `PLAY`. Connect with any RTSP client, e.g.:

```bash
ffplay rtsp://<pi-ip>:8554/stream
```

`PipelineController` lazily starts the capture→encode→packetize→send chain on the first `PLAY` (ref-counted, so it stays running while any client is watching) and stops it again once the last client `TEARDOWN`s or is reaped as an orphan (~60s of inactivity, no keepalive). `DESCRIBE` returns a real SDP built from the encoder's actual cached SPS/PPS — see `roadmap.md`'s Phase 20 entry for how that's primed if `DESCRIBE` arrives before anyone has ever played.

The app streams until you press **ENTER** (or the RTSP server is stopped), at which point it shuts down every stage in producer → consumer order.

## Project layout

```
include/          Public headers, mirrors src/ layout
  capture/         libcamera-based CSI capture
  encoder/         BCM2835 HW H.264 encoder + encoder thread
  decoder/         BCM2835 HW H.264 decoder (receiver side)
  frame/           Raw/encoded frame types, pools, and queues
  parser/          H.264 NAL unit parser, SPS/PPS cache
  rtp/             RTP packet type/pool/queue, packetizer/depacketizer, jitter buffer, RTCP
  network/         UDP sender (multi-destination) + sender thread, control channel
  rtsp/            RTSP server, session registry, pipeline controller (lazy start/stop)
  writer/          Dashcam-style circular H.264 recorder (receiver side)
  common/          Logger, time utils, app-wide state, base64

src/               Implementation, mirrors include/ layout
  main.cpp         Sender: hardware/pool init, RTSP server, control channel, RTCP
  main_receiver.cpp  Receiver: jitter buffer, depacketizer, decoder, circular recorder

CMakeLists.txt     Build configuration (camera_app + camera_receiver targets)
build.sh           Clean configure + build helper script
roadmap.md         Phase-by-phase status and implementation evidence (source of truth)
```

### Key modules

- **`capture/camera_capture`** — Not a dedicated thread; libcamera's own event loop drives capture and pushes frames into `raw_frame_queue` via a callback.
- **`encoder/encoder_thread` + `encoder/bcm2835_encoder`** — Consumes `raw_frame_queue`, feeds frames to the BCM2835 hardware encoder (`/dev/video11`, V4L2 M2M), and pushes H.264 output into `encoded_frame_queue`. Bitrate is adjustable at runtime (3-tier adaptive bitrate, driven by receiver-reported loss).
- **`parser/h264_nal_parser`** — Iterator-style NAL unit splitting (`h264_nal_parser_init` / `h264_nal_parser_next`), used by the RTP packetizer.
- **`parser/sps_pps_cache`** — Keeps the most recently seen SPS/PPS NAL bytes around so the RTSP server's `DESCRIBE` can build a real SDP `sprop-parameter-sets` attribute instead of a placeholder.
- **`rtp/rtp_packetizer` + `rtp/rtp_packetizer_thread`** — Consumes `encoded_frame_queue`, splits each frame into NAL units (mirroring SPS/PPS into the cache above as it goes), packetizes into RTP packets (FU-A fragmentation per RFC 6184), and pushes them into `rtp_packet_queue`.
- **`rtp/rtcp_sender_thread` / `rtp/rtcp_receiver_thread`** — RTCP Sender/Receiver Reports per RFC 3550, multiplexed onto the existing RTP/control ports (RFC 5761 rtcp-mux).
- **`network/udp_sender` + `network/udp_sender_thread`** — Consumes `rtp_packet_queue` and fans each packet out to every currently-registered destination (keyed by RTSP `session_id` — one per `PLAY`ing client), rather than a single fixed destination.
- **`rtsp/rtsp_server`** — TCP RTSP/1.0 server (`OPTIONS`/`DESCRIBE`/`SETUP`/`PLAY`/`TEARDOWN`), backed by `rtsp/rtsp_session_registry` (max 5 concurrent sessions, orphan reaper) and `rtsp/pipeline_controller` (ref-counted lazy start/stop of the whole capture→encode→packetize→send chain).
- **`writer/circular_h264_writer`** — Receiver-side dashcam-style recorder: rotates fixed-duration segments, deletes the oldest once a storage cap is hit.

## Roadmap

See [`roadmap.md`](./roadmap.md) for the full 24-phase roadmap with per-phase implementation evidence. Current focus: **Phase 22 (WebRTC)**, broken into sub-phases 22.1-22.6 (signaling server → WebRTC-compatible SDP → ICE → DTLS handshake → SRTP → end-to-end integration). None of 22.1-22.6 are implemented yet.

## Design notes

- **Thread lifecycle**: each stage owns its own running flag rather than sharing one app-wide flag, so stages can be started/stopped independently per RTSP session without racing each other.
- **Zero-copy pipeline**: frames/packets are passed by pointer between pool-backed queues; each stage only copies data once, at its own boundary with hardware/network.
- **Lazy pipeline start**: the capture→encode→packetize→send chain only runs while at least one RTSP session is `PLAY`ing (ref-counted via `PipelineController`) — not from the moment `camera_app` starts.
- **Shutdown order**: camera stopped first (no more producers), then encoder thread, then RTP packetizer thread, then UDP sender thread — each stage drains its input queue before exiting, avoiding deadlocks on a queue that will never receive another item.
