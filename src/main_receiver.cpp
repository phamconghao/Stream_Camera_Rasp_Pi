#include <iostream>
#include <cstdlib>
#include <cstdint>

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
#include "keyframe_requester.h"

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
 *     |    (also dumps the reassembled elementary stream to received.h264 for offline verification)
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
 * CONTROL CHANNEL (Phase 18 - packet loss recovery), separate from the
 * RTP data path above: network/keyframe_requester.{h,cpp} sends a tiny
 * UDP message back to the sender's keyframe_listener_thread whenever
 * rtp_jitter_buffer reports lost packets (see the call in
 * rtp_depacketizer_thread.cpp), asking it to force an IDR immediately
 * instead of waiting for the next regularly-scheduled one.
 */

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
    const char *sender_ip = (argc > 3) ? argv[3] : "127.0.0.1";
    uint16_t control_port = (argc > 4) ? static_cast<uint16_t>(std::atoi(argv[4])) : 5005;

    // This pipeline is fixed at 640x480, same as the sender - see the
    // resolution note in bcm2835_decoder.cpp for why this isn't
    // negotiated dynamically yet.
    const int width = 640;
    const int height = 480;

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

    if (keyframe_requester_init(sender_ip, control_port) < 0)
    {
        return -1;
    }

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

    if (rtp_depacketizer_thread_start() < 0)
    {
        return -1;
    }

    if (udp_receiver_thread_start(listen_port) < 0)
    {
        return -1;
    }

    std::cout << "Listening for RTP on port " << listen_port
              << ", writing decoded frames to " << output_path << std::endl;
    std::cout << "Keyframe requests will be sent to " << sender_ip << ":" << control_port << std::endl;
    std::cout << "Press ENTER to exit..." << std::endl;

    std::cin.get();

    // Shutdown in strict producer -> consumer order so each stage can
    // drain and exit cleanly instead of deadlocking on a queue/buffer
    // that will never receive another item:
    //   1. Stop the UDP receiver (no more packets enter the jitter buffer)
    //   2. Stop the depacketizer thread (drains the jitter buffer, then exits)
    //   3. Stop the decoder thread (drains the encoded queue, then exits)
    //   4. Stop the YUV writer thread (drains the raw queue, then exits)
    g_running = false;

    udp_receiver_thread_stop();

    rtp_depacketizer_thread_stop();

    decoder_thread_stop();

    yuv_writer_stop();

    encoded_frame_queue_cleanup();
    encoded_frame_pool_cleanup();

    raw_frame_queue_cleanup();
    raw_frame_pool_cleanup();

    rtp_jitter_buffer_cleanup();
    rtp_packet_pool_cleanup();

    keyframe_requester_cleanup();

    return 0;
}
