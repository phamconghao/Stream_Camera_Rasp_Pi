#include <iostream>
#include <pthread.h>
#include <atomic>
#include <cstdlib>
#include <cstdint>

#include "app_state.h"
#include "camera_capture.h"
#include "raw_frame_pool.h"
#include "raw_frame_queue.h"
#include "encoded_frame_pool.h"
#include "encoded_frame_queue.h"
#include "encoder_thread.h"
#include "bcm2835_encoder.h"
#include "h264_writer.h"
#include "rtp_packetizer.h"
#include "rtp_packetizer_thread.h"
#include "rtp_packet_pool.h"
#include "rtp_packet_queue.h"
#include "udp_sender_thread.h"
#include "keyframe_listener_thread.h"

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
 * hardware encoder). Each thread stage owns its own independent
 * start/stop running flag (see e.g. g_encoder_running in
 * encoder_thread.cpp) - app_state::g_running (below) is a separate,
 * app-wide flag only main() writes to.
 *
 * CONTROL CHANNEL (Phase 18 - packet loss recovery), separate from the
 * RTP data path above: network/keyframe_listener_thread.{h,cpp} listens
 * on `control_port` for a tiny UDP message from the receiver's
 * keyframe_requester (sent whenever its jitter buffer detects lost
 * packets), and calls bcm2835_encoder_force_keyframe() so the receiver
 * can recover a clean picture quickly instead of waiting for the next
 * regularly-scheduled IDR.
 *
 * NOT YET IMPLEMENTED (see roadmap): RTSP Server (would replace the
 * hardcoded dest_ip/dest_port below with per-client negotiation) and
 * full RTCP (this project only has a minimal one-way keyframe-request
 * signal so far, not RFC 3550 Receiver/Sender Reports).
 */

// Dead code: an earlier, simpler alternative to rtp_packetizer_thread
// that just logged encoded frames. Not started anywhere in main() below
// (the pthread_create call for it is commented out further down) - kept
// only as a reference of the old, no-longer-used minimal consumer.
static std::atomic<bool> g_consumer_running(true);

void *consumer_thread(void *)
{
    while (g_consumer_running)
    {
        encoded_frame_t *frame = encoded_frame_queue_pop();

        if (!frame)
        {
            continue;
        }

        std::cout << "[CONSUMER] size = " << frame->size << " seq = " << frame->sequence << std::endl;

        encoded_frame_pool_release(frame);
    }

    return nullptr;
}

int main(int argc, char **argv)
{
    // Destination for the UDP sender. Hardcoded/CLI-provided for now;
    // once the RTSP server exists, it will negotiate the real client
    // address per session (SETUP request) instead of a fixed target.
    const char *dest_ip = (argc > 1) ? argv[1] : "127.0.0.1";
    uint16_t dest_port = (argc > 2) ? static_cast<uint16_t>(std::atoi(argv[2])) : 5004;

    // Control channel port this sender listens on for keyframe-request
    // datagrams from the receiver (see keyframe_listener_thread.h).
    // Independent of dest_port (which is where WE send RTP data TO) -
    // this is where WE receive control messages FROM. Must match the
    // control_port argument passed to camera_receiver.
    uint16_t control_port = (argc > 3) ? static_cast<uint16_t>(std::atoi(argv[3])) : 5005;

    // App-level flag: only main() (or a future signal handler installed
    // by main()) writes to this. Each thread module manages its own
    // independent running flag for start/stop, so they can be controlled
    // separately later (per RTSP-client sessions, etc.).
    g_running = true;

    // Initialize every pool/queue BEFORE starting any thread that could
    // touch them - each stage's pool/queue must exist before its
    // producer or consumer thread can safely run.
    if (raw_frame_pool_init() < 0)
    {
        return -1;
    }

    if (raw_frame_queue_init() < 0)
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

    if (rtp_packet_pool_init() < 0)
    {
        return -1;
    }

    if (rtp_packet_queue_init() < 0)
    {
        return -1;
    }

    // h264_writer would write the encoded stream straight to a local
    // .h264 file - an alternative/earlier consumer of encoded_frame_queue
    // to the current RTP path. Disabled since the pipeline now streams
    // over RTP/UDP instead of (or as well as) writing to disk.
    // if (h264_writer_start() < 0)
    // {
    //     return -1;
    // }

    if (camera_capture_init() < 0)
    {
        return -1;
    }

    if (bcm2835_encoder_init(640, 480) < 0)
    {
        return -1;
    }

    // pthread_t consumer_tid;

    // pthread_create(&consumer_tid, nullptr, consumer_thread, nullptr);

    // Start order: consumer threads first (encoder_thread, then further
    // downstream rtp_packetizer_thread/udp_sender_thread), THEN start
    // the camera producing frames - so nothing is ever pushed into a
    // queue before its consumer thread exists to eventually drain it.
    if (encoder_thread_start() < 0)
    {
        return -1;
    }

    camera_capture_start();

    if (rtp_packetizer_thread_start() < 0)
    {
        return -1;
    }

    if (udp_sender_thread_start(dest_ip, dest_port) < 0)
    {
        return -1;
    }

    if (keyframe_listener_thread_start(control_port) < 0)
    {
        return -1;
    }

    std::cout << "Press ENTER to exit..." << std::endl;

    std::cin.get();

    // App-wide shutdown signal first (for any future code that only
    // polls g_running, e.g. a SIGINT-driven main loop). Each thread is
    // then stopped explicitly and independently, in producer -> consumer
    // order so each stage can drain and exit cleanly instead of
    // deadlocking on a queue that will never receive another item:
    //   1. Stop the camera (no more raw frames produced)
    //   2. Stop the encoder thread (drains raw queue, then exits)
    //   3. Stop the RTP packetizer thread (drains encoded queue, then exits)
    //   4. Stop the UDP sender thread (drains RTP packet queue, then exits)
    //   5. Stop the keyframe listener thread (independent control channel,
    //      order relative to the others above doesn't matter)
    g_running = false;

    camera_capture_stop();
    // camera_capture_cleanup();

    encoder_thread_stop();

    rtp_packetizer_thread_stop();

    udp_sender_thread_stop();

    keyframe_listener_thread_stop();

    encoded_frame_queue_cleanup();
    encoded_frame_pool_cleanup();

    rtp_packet_queue_cleanup();
    rtp_packet_pool_cleanup();

    raw_frame_queue_cleanup();
    raw_frame_pool_cleanup();

    // bcm2835_encoder_encode_file("frame_000.yuv", "output.h264");

    bcm2835_encoder_cleanup();

    return 0;
}