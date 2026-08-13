#include <iostream>
#include <cstdlib>
#include <cstdint>

#include "app_state.h"
#include "control_listener_thread.h"
#include "rtcp_sender_thread.h"
#include "rtsp_server.h"
#include "pipeline_controller.h"

/**
 * ============================================================================
 * FULL PIPELINE MAP (see individual files for per-stage detail comments)
 * ============================================================================
 *
 *   Camera (libcamera)
 *     |
 *   Capture         <- camera_capture.{h,cpp}            (no dedicated thread; driven by libcamera's own callback)
 *     |
 *   Raw Frame Pool/Queue   <- frame/raw_frame*.{h,cpp}
 *     |
 *   Encoder Thread  <- encoder/encoder_thread.{h,cpp} + encoder/bcm2835_encoder.{h,cpp}
 *     |
 *   Encoded Frame Pool/Queue  <- frame/encoded_frame*.{h,cpp}
 *     |
 *   RTP Packetizer Thread  <- rtp/rtp_packetizer_thread.{h,cpp}
 *     |    (uses parser/h264_nal_parser.{h,cpp} + rtp/rtp_packetizer.{h,cpp})
 *     |
 *   RTP Packet Pool/Queue  <- rtp/rtp_packet*.{h,cpp}
 *     |
 *   UDP Sender Thread  <- network/udp_sender_thread.{h,cpp} + network/udp_sender.{h,cpp}
 *     |
 *   Network -> RTSP/RTP Client
 *
 * Every arrow above is "producer pushes pointer, consumer pops pointer" -
 * frame/packet bytes are never copied between pipeline stages (only
 * copied once, when captured from the camera / read out of the
 * hardware encoder).
 *
 * PHASE 20 step 3 (lazy pipeline lifecycle): unlike every earlier phase,
 * main() below no longer starts the data pipeline above unconditionally
 * at process startup. rtsp/pipeline_controller.{h,cpp} now owns that
 * entirely - main() just prepares it (hardware init, pool/queue alloc)
 * via pipeline_controller_init(), then the RTSP server's PLAY/TEARDOWN
 * handlers (rtsp_server.cpp) call pipeline_controller_ensure_running()/
 * _release() as clients come and go. The control channel, RTCP, and the
 * RTSP control plane itself are NOT lazy - they listen continuously
 * from process startup, independent of whether the data pipeline is
 * currently running, so a client can always connect/DESCRIBE/SETUP even
 * before anyone has PLAYed.
 *
 * CONTROL CHANNEL (Phase 18 packet-loss recovery + adaptive bitrate),
 * separate from the RTP data path above: network/control_listener_thread.{h,cpp}
 * listens on `control_port` for two kinds of UDP messages from the
 * receiver's control_channel (network/control_channel.h):
 *   - keyframe request (sent whenever the receiver's jitter buffer
 *     detects lost packets) -> bcm2835_encoder_force_keyframe(), so the
 *     receiver recovers a clean picture quickly instead of waiting for
 *     the next regularly-scheduled IDR.
 *   - loss report (sent periodically by the receiver's
 *     loss_reporter_thread) -> maps the reported loss rate to a target
 *     bitrate tier and applies it via bcm2835_encoder_set_bitrate() if
 *     it changed, so the stream backs off automatically under bad
 *     network conditions instead of continuing to send at a rate the
 *     link can't sustain.
 *
 * RTCP (Phase 19): network/rtcp_sender_thread.{h,cpp} periodically
 * injects a Sender Report (RFC 3550 6.4.1) into the SAME socket
 * udp_sender_thread uses for RTP data (rtcp-mux, RFC 5761 - no new
 * port). This is genuine RFC 3550 wire format (interoperable with
 * tools like Wireshark), separate from and in addition to the ad-hoc
 * control_channel messages above.
 *
 * PHASE 20 step 4 (this commit): RTP fan-out - udp_sender now sends
 * every packet to the destination of EACH currently-PLAYING RTSP
 * session (rtsp_server.cpp's handle_play()/handle_teardown() register/
 * deregister them by session_id), instead of the single fixed
 * dest_ip/dest_port CLI argument earlier phases used for point-to-point
 * testing - that argument is gone below. See udp_sender.h and
 * pipeline_controller.h for the full picture.
 */

int main(int argc, char **argv)
{
    // Control channel port this sender listens on for keyframe-request
    // datagrams from the receiver (see control_listener_thread.h).
    uint16_t control_port = (argc > 1) ? static_cast<uint16_t>(std::atoi(argv[1])) : 5005;

    // TCP port the RTSP control plane listens on (Phase 20).
    uint16_t rtsp_port = (argc > 2) ? static_cast<uint16_t>(std::atoi(argv[2])) : 8554;

    // App-level flag: only main() (or a future signal handler installed
    // by main()) writes to this. Each thread module manages its own
    // independent running flag for start/stop, so they can be controlled
    // separately later (per RTSP-client sessions, etc.).
    g_running = true;

    // Hardware init + pool/queue allocation only - does NOT start
    // streaming yet. The data pipeline itself is started lazily, on
    // the first RTSP PLAY - see pipeline_controller.h. RTP destinations
    // are no longer fixed here either (see udp_sender.h) - each PLAYING
    // RTSP session registers its own.
    if (pipeline_controller_init() < 0)
    {
        return -1;
    }

    // These three listen continuously from startup, independent of
    // whether any client has PLAYed yet.
    if (control_listener_thread_start(control_port) < 0)
    {
        return -1;
    }

    if (rtcp_sender_thread_start() < 0)
    {
        return -1;
    }

    if (rtsp_server_start(rtsp_port) < 0)
    {
        return -1;
    }

    std::cout << "RTSP server ready at rtsp://<this-pi-ip>:" << rtsp_port << "/stream" << std::endl;
    std::cout << "Press ENTER to exit..." << std::endl;

    std::cin.get();

    g_running = false;

    // Stop the always-on services first, then let pipeline_controller
    // force-stop the data pipeline if any RTSP client was still PLAYing
    // (its cleanup() is defensive about this - see pipeline_controller.cpp).
    control_listener_thread_stop();

    rtcp_sender_thread_stop();

    rtsp_server_stop();

    pipeline_controller_cleanup();

    return 0;
}
