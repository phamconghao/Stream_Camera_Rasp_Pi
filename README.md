# Stream_Camera_Rasp_Pi

A C++ pipeline for streaming CSI camera video from a **Raspberry Pi 4B** over the network, using **libcamera** for capture, the **BCM2835 V4L2 M2M hardware encoder** for H.264, and **RTP/UDP** for delivery.

```
Camera (libcamera) → Capture → Raw Frame Pool/Queue → Encoder Thread (BCM2835 HW H.264)
    → Encoded Frame Pool/Queue → RTP Packetizer Thread (H.264 NAL parser) → RTP Packet Pool/Queue
    → UDP Sender Thread → Network → RTSP/RTP Client
```

Every stage hands off a **pointer**, never a copy — frame/packet bytes are copied exactly once, when read out of the camera or the hardware encoder.

> Branch: `csi_camera_streaming`

## Status

| Stage | Status |
|---|---|
| CSI camera capture (libcamera) | ✅ Implemented |
| Raw/Encoded frame pools & queues | ✅ Implemented |
| BCM2835 hardware H.264 encoder (`/dev/video11`) | ✅ Implemented |
| H.264 NAL parser | ✅ Implemented |
| RTP packetizer thread | ✅ Implemented |
| UDP sender thread | ✅ Implemented, verified via loopback test |
| RTSP server (per-client `SETUP`/session negotiation) | ⏳ Not started |
| RTCP (receiver feedback / loss reporting) | ⏳ Not started |

Each pipeline stage owns an independent `static std::atomic<bool>` running flag (e.g. `g_encoder_running`, `g_capture_running`), separate from the app-wide `app_state::g_running` flag that only `main()` writes to. This keeps shutdown clean and paves the way for per-RTSP-session start/stop of individual threads.

## Requirements

- Raspberry Pi 4B (BCM2835/BCM2711 V4L2 M2M H.264 encoder at `/dev/video11`)
- A CSI camera module supported by `libcamera`
- CMake ≥ 3.16
- GCC with C++17 support
- `libcamera` development package (`libcamera-dev`) discoverable via `pkg-config`
- `pthread`

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
./build/camera_app [dest_ip] [dest_port]
```

- `dest_ip` — UDP destination IP (default: `127.0.0.1`)
- `dest_port` — UDP destination port (default: `5004`)

Example, streaming to a client at `192.168.1.50:5004`:

```bash
./build/camera_app 192.168.1.50 5004
```

The app initializes the camera at 640x480 YUV420, starts every pipeline thread, and streams until you press **ENTER**, at which point it shuts down every stage in producer → consumer order (camera → encoder → RTP packetizer → UDP sender) so each stage drains its queue before exiting.

> `dest_ip`/`dest_port` are fixed for now. Once the RTSP server exists, it will negotiate the client address per session via `SETUP` instead of a hardcoded destination.

## Project layout

```
include/          Public headers, mirrors src/ layout
  capture/         libcamera-based CSI capture
  encoder/         BCM2835 HW H.264 encoder + encoder thread
  frame/           Raw/encoded frame types, pools, and queues
  parser/          H.264 NAL unit parser
  rtp/             RTP packet type/pool/queue, packetizer, packetizer thread
  network/         UDP sender + sender thread
  writer/          Optional local .h264 file writer (currently unused by main)
  common/          Logger, time utils, app-wide state

src/               Implementation, mirrors include/ layout
  main.cpp         Wires up and drives the full pipeline

CMakeLists.txt     Build configuration
build.sh           Clean configure + build helper script
```

### Key modules

- **`capture/camera_capture`** — Not a dedicated thread; libcamera's own event loop drives capture and pushes frames into `raw_frame_queue` via a callback.
- **`encoder/encoder_thread` + `encoder/bcm2835_encoder`** — Consumes `raw_frame_queue`, feeds frames to the BCM2835 hardware encoder (`/dev/video11`, V4L2 M2M), and pushes H.264 output into `encoded_frame_queue`.
- **`parser/h264_nal_parser`** — Iterator-style NAL unit splitting (`h264_nal_parser_init` / `h264_nal_parser_next`), used by the RTP packetizer.
- **`rtp/rtp_packetizer` + `rtp/rtp_packetizer_thread`** — Consumes `encoded_frame_queue`, splits each frame into NAL units, packetizes into RTP packets, and pushes them into `rtp_packet_queue`.
- **`network/udp_sender` + `network/udp_sender_thread`** — Consumes `rtp_packet_queue` and sends each packet over UDP to the configured destination. Verified via a live loopback test against a Python UDP listener on `127.0.0.1:6000`.
- **`writer/h264_writer`** — An alternative/earlier consumer that would write the encoded stream directly to a local `.h264` file instead of streaming it. Present but disabled in `main.cpp` now that the pipeline streams over RTP/UDP.

## Roadmap

1. **RTSP server** — negotiate per-client sessions (`DESCRIBE`/`SETUP`/`PLAY`/`TEARDOWN`) instead of the current hardcoded UDP destination; start/stop individual pipeline threads per session.
2. **RTCP** — receiver feedback and loss reporting.

## Design notes

- **Thread lifecycle**: each stage owns its own running flag rather than sharing one app-wide flag, so stages can be started/stopped independently (e.g. per RTSP session) without racing each other.
- **Zero-copy pipeline**: frames/packets are passed by pointer between pool-backed queues; each stage only copies data once, at its own boundary with hardware/network.
- **Shutdown order**: camera stopped first (no more producers), then encoder thread, then RTP packetizer thread, then UDP sender thread — each stage drains its input queue before exiting, avoiding deadlocks on a queue that will never receive another item.
