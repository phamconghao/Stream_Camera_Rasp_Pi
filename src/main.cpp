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

    // App-level flag: only main() (or a future signal handler installed
    // by main()) writes to this. Each thread module manages its own
    // independent running flag for start/stop, so they can be controlled
    // separately later (per RTSP-client sessions, etc.).
    g_running = true;

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
    g_running = false;

    camera_capture_stop();
    // camera_capture_cleanup();

    encoder_thread_stop();

    rtp_packetizer_thread_stop();

    udp_sender_thread_stop();

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