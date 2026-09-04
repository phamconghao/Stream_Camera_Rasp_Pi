#include <iostream>
#include <cstdlib>
#include <cstdint>
#include <string>

#include "app_state.h"
#include "rtp_packet_pool.h"
#include "rtp_jitter_buffer.h"
#include "udp_receiver_thread.h"
#include "rtp_depacketizer_thread.h"
#include "encoded_frame_pool.h"
#include "encoded_frame_queue.h"
#include "decoder_thread.h"
#include "raw_frame_pool.h"
#include "raw_frame_queue.h"
#include "yuv_writer.h"
#include "control_channel.h"
#include "loss_reporter_thread.h"
#include "circular_h264_writer.h"
#include "rtcp_receiver_thread.h"
#include "rtcp_receiver_stats.h"
#include <cstring>

/**
 * ============================================================================
 * FULL RECEIVER PIPELINE MAP (mirror image of main.cpp's sender pipeline)
 * ============================================================================
 *
 *   Network <- RTP/UDP from the sender (see main.cpp / udp_sender)
 *     |
 *   UDP Receiver Thread     <- network/udp_receiver_thread.{h,cpp} + network/udp_receiver.{h,cpp}
 *     |
 *   RTP Packet Pool          <- rtp/rtp_packet_pool.{h,cpp}          (same pool type as the sender side)
 *     |
 *   RTP Jitter Buffer        <- rtp/rtp_jitter_buffer.{h,cpp}        (sequence reordering + loss detection)
 *     |
 *   RTP Depacketizer Thread  <- rtp/rtp_depacketizer_thread.{h,cpp} + rtp/rtp_depacketizer.{h,cpp}
 *     |    (also mirrors each completed access unit into circular_h264_writer -
 *     |     Idea #4, bounded rotating "dashcam" storage; see writer/circular_h264_writer.h)
 *     |
 *   Encoded Frame Pool/Queue <- frame/encoded_frame*.{h,cpp}          (same struct type as the sender side)
 *     |
 *   Decoder Thread            <- decoder/decoder_thread.{h,cpp} + decoder/bcm2835_decoder.{h,cpp}
 *     |
 *   Raw Frame Pool/Queue      <- frame/raw_frame*.{h,cpp}             (same struct type as the sender side)
 *     |
 *   YUV Writer Thread         <- writer/yuv_writer.{h,cpp}
 *     |
 *   received.yuv
 *
 * Every arrow is "producer pushes pointer, consumer pops pointer" -
 * same zero-copy pool/queue design as the sender pipeline. This binary
 * intentionally reuses the sender's rtp_packet_t/encoded_frame_t/
 * raw_frame_t types and their pools/queues (they're just data, no
 * sender-specific state), which is why it's a separate executable
 * target (camera_receiver) rather than folded into main.cpp - the two
 * pipelines never run in the same process.
 *
 * CONTROL CHANNEL (packet-loss recovery + adaptive bitrate), separate
 * from the RTP data path above: network/control_channel.{h,cpp}
 * sends two kinds of UDP messages back to the sender's
 * control_listener_thread:
 *   - keyframe request, whenever rtp_jitter_buffer reports lost packets
 *     (see the call in rtp_depacketizer_thread.cpp), asking the sender
 *     to force an IDR immediately instead of waiting for the next
 *     regularly-scheduled one.
 *   - loss report, sent periodically by loss_reporter_thread.{h,cpp}
 *     (reads rtp_jitter_buffer's cumulative stats), driving the
 *     sender's adaptive bitrate decision.
 *
 * RTCP: rtp/rtcp_receiver_thread.{h,cpp} periodically sends a genuine
 * RFC 3550 Receiver Report back to the sender, multiplexed onto
 * control_channel's already-open socket (RFC 5761 rtcp-mux - no new
 * port). rtp/rtcp_receiver_stats.{h,cpp} does the actual bookkeeping
 * (interarrival jitter, extended sequence numbers, LSR/DLSR), fed by
 * udp_receiver_thread per packet and by incoming RTCP SR packets (sent
 * by the sender's rtcp_sender_thread, arriving multiplexed with RTP
 * data on `listen_port`).
 */

// Must match rtp_packetizer_thread's RTP_SSRC constant on the sender
// side - this identifies which stream the RR is about. g_reporter_ssrc
// is an arbitrary, distinct identifier for this receiver (a real
// implementation would randomize this per RFC 3550 section 8.1; fixed
// here since there's only ever one receiver).
static constexpr uint32_t RTCP_VIDEO_SSRC = 0x12345678;
static constexpr uint32_t RTCP_REPORTER_SSRC = 0x87654321;

int main(int argc, char **argv)
{
    // Port this Pi listens on for incoming RTP/UDP packets - should
    // match the dest_port the sender (main.cpp) was started with.
    uint16_t listen_port = (argc > 1) ? static_cast<uint16_t>(std::atoi(argv[1])) : 5004;
    const char *output_path = (argc > 2) ? argv[2] : "received.yuv";

    // Where to send keyframe-request control messages: the sender's IP
    // and the control_port it was started with (see main.cpp). Same
    // "hardcoded/CLI-provided until RTSP negotiates it" caveat as
    // dest_ip/dest_port on the sender side.
    const char *sender_ip = (argc > 3) ? argv[3] : "192.168.1.100";
    uint16_t control_port = (argc > 4) ? static_cast<uint16_t>(std::atoi(argv[4])) : 5005;

    // PHASE 23.2: must match camera_app's CAMERA_CONTROL_SECRET exactly
    // - see the matching check/comment in main.cpp for why this is an
    // env var rather than a positional argv.
    const char *control_secret_env = std::getenv("CAMERA_CONTROL_SECRET");
    if (control_secret_env == nullptr || control_secret_env[0] == '\0')
    {
        std::cerr << "CAMERA_CONTROL_SECRET environment variable must be set "
                     "(shared secret for control-channel HMAC authentication, "
                     "see docs-security-threat-model.md) - refusing to start "
                     "with no authentication on the control channel.\n";
        return -1;
    }
    std::string control_secret(control_secret_env);

    // Idea #4 (dashcam-style recording): directory circular_h264_writer
    // rotates segments into. Pass "none" to disable recording entirely.
    // Defaults to keeping the last 10 minutes (10 segments x 60s) -
    // strictly bounded, unlike the single ever-growing received.h264
    // dump this replaced.
    const char *recording_dir = (argc > 5) ? argv[5] : "recordings";
    int segment_duration_sec = (argc > 6) ? std::atoi(argv[6]) : 60;
    int max_segments = (argc > 7) ? std::atoi(argv[7]) : 10;
    bool recording_disabled = (std::strcmp(recording_dir, "none") == 0);

    // This pipeline is fixed at 1920x1080 (FHD), same as the sender -
    // see the resolution note in bcm2835_decoder.cpp for why this
    // isn't negotiated dynamically yet.
    const int width = 1920;
    const int height = 1080;

    g_running = true;

    // Initialize every pool/queue BEFORE starting any thread that could
    // touch them, same convention as main.cpp.
    if (rtp_packet_pool_init() < 0)
    {
        return -1;
    }

    if (rtp_jitter_buffer_init() < 0)
    {
        return -1;
    }

    if (encoded_frame_pool_init() < 0)
    {
        return -1;
    }

    if (encoded_frame_queue_init() < 0)
    {
        return -1;
    }

    if (raw_frame_pool_init() < 0)
    {
        return -1;
    }

    if (raw_frame_queue_init() < 0)
    {
        return -1;
    }

    if (control_channel_init(sender_ip, control_port, control_secret) < 0)
    {
        return -1;
    }

    rtcp_receiver_stats_init();

    // Start order: consumer threads first (furthest downstream to
    // furthest upstream), THEN the producer (UDP receiver) - so nothing
    // is ever pushed into a queue/buffer before its consumer thread
    // exists to eventually drain it. Mirrors main.cpp's start order.
    if (yuv_writer_start(output_path) < 0)
    {
        return -1;
    }

    if (decoder_thread_start(width, height) < 0)
    {
        return -1;
    }

    if (rtp_depacketizer_thread_start(recording_disabled ? nullptr : recording_dir,
                                       segment_duration_sec, max_segments) < 0)
    {
        return -1;
    }

    if (udp_receiver_thread_start(listen_port) < 0)
    {
        return -1;
    }

    if (loss_reporter_thread_start() < 0)
    {
        return -1;
    }

    if (rtcp_receiver_thread_start(RTCP_REPORTER_SSRC, RTCP_VIDEO_SSRC) < 0)
    {
        return -1;
    }

    std::cout << "Listening for RTP on port " << listen_port
              << ", writing decoded frames to " << output_path << std::endl;
    std::cout << "Keyframe requests and loss reports will be sent to "
              << sender_ip << ":" << control_port << std::endl;
    if (!recording_disabled)
    {
        std::cout << "Recording last " << (segment_duration_sec * max_segments / 60)
                  << " minute(s) to " << recording_dir << "/ (" << max_segments
                  << " x " << segment_duration_sec << "s segments)" << std::endl;
    }
    std::cout << "Press ENTER to exit..." << std::endl;

    std::cin.get();

    // Shutdown in strict producer -> consumer order so each stage can
    // drain and exit cleanly instead of deadlocking on a queue/buffer
    // that will never receive another item:
    //   1. Stop the UDP receiver (no more packets enter the jitter buffer)
    //   2. Stop the depacketizer thread (drains the jitter buffer, then exits)
    //   3. Stop the decoder thread (drains the encoded queue, then exits)
    //   4. Stop the YUV writer thread (drains the raw queue, then exits)
    //   5. Stop the loss reporter thread (independent timer, order
    //      relative to the others above doesn't matter)
    //   6. Stop the RTCP receiver thread (also independent)
    g_running = false;

    udp_receiver_thread_stop();

    rtp_depacketizer_thread_stop();

    decoder_thread_stop();

    yuv_writer_stop();

    loss_reporter_thread_stop();

    rtcp_receiver_thread_stop();

    encoded_frame_queue_cleanup();
    encoded_frame_pool_cleanup();

    raw_frame_queue_cleanup();
    raw_frame_pool_cleanup();

    rtp_jitter_buffer_cleanup();
    rtp_packet_pool_cleanup();

    control_channel_cleanup();

    return 0;
}
