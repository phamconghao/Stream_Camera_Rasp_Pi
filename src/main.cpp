#include <iostream>
#include <pthread.h>
#include <atomic>

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

int main()
{
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

    pthread_t rtp_tid;
    pthread_create(&rtp_tid, nullptr, rtp_packetizer_thread_func, nullptr);

    std::cout << "Press ENTER to exit..." << std::endl;

    std::cin.get();

    pthread_join(rtp_tid, nullptr);
    // h264_writer_stop();
    camera_capture_stop();

    // camera_capture_cleanup();

    encoder_thread_stop();

    // g_consumer_running = false;

    // pthread_join( consumer_tid, nullptr);

    encoded_frame_queue_cleanup();
    encoded_frame_pool_cleanup();

    raw_frame_queue_cleanup();
    raw_frame_pool_cleanup();

    // bcm2835_encoder_encode_file("frame_000.yuv", "output.h264");

    bcm2835_encoder_cleanup();

    return 0;
}