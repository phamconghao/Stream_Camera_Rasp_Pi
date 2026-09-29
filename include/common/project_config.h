#ifndef __PROJECT_CONFIG_H__
#define __PROJECT_CONFIG_H__

#include <cstdint>

/**
 * Central place for this project's tunable settings that would
 * otherwise be hardcoded/duplicated across multiple files - camera
 * resolution, default ports, adaptive-bitrate tiers. Anything here is
 * a value an operator/maintainer might reasonably want to change in
 * one place; it is NOT a dumping ground for every constant in the
 * codebase - implementation details that only one file cares about
 * (pool sizes, V4L2 enum choices, etc.) stay local to that file.
 */

// Camera capture / encode resolution. camera_capture.cpp (libcamera
// StreamConfiguration), bcm2835_encoder.cpp (V4L2 OUTPUT/CAPTURE
// format), raw_frame.h's MAX_RAW_FRAME_SIZE, and main_receiver.cpp's
// decode-side width/height must all agree on this exactly - the whole
// pipeline is fixed-resolution end-to-end, not negotiated dynamically
// (see bcm2835_decoder.cpp's resolution note for why). Changing this
// is the one line that needs to change; every other file references it.
constexpr int CAMERA_WIDTH = 1920;
constexpr int CAMERA_HEIGHT = 1080;

// Default TCP/UDP ports (all overridable via argv - see main.cpp).
constexpr uint16_t DEFAULT_CONTROL_PORT = 5005;   // UDP: keyframe-request/bitrate-feedback from receiver
constexpr uint16_t DEFAULT_RTSP_PORT = 8554;      // TCP: RTSP control plane
constexpr uint16_t DEFAULT_SIGNALING_PORT = 8765; // TCP: WebRTC signaling (WebSocket)
constexpr uint16_t DEFAULT_ADMIN_PORT = 80;       // TCP: admin login/dashboard HTTP

// Adaptive-bitrate tiers (see control_listener_thread.cpp) - deliberately
// a coarse 3-step scheme, not a smooth/continuous congestion-control
// curve. loss < 1% -> HIGH, 1% <= loss < 5% -> MEDIUM, loss >= 5% -> LOW.
constexpr uint32_t BITRATE_HIGH_BPS = 2000000;
constexpr uint32_t BITRATE_MEDIUM_BPS = 1000000;
constexpr uint32_t BITRATE_LOW_BPS = 500000;

#endif // __PROJECT_CONFIG_H__
