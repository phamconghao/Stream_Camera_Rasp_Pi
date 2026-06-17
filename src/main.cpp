#include <iostream>

#include "camera_capture.h"
#include "raw_frame_pool.h"
#include "raw_frame_queue.h"

static std::atomic<bool> g_running(true);

void *consumer_thread(void *)
{
    while (g_running)
    {
        raw_frame_t *frame = raw_frame_queue_pop();

        if (!frame)
        {
            continue;
        }

        std::cout << "[CONSUMER] size = " << frame->size << " seq = " << frame->sequence << std::endl;

        raw_frame_pool_release(frame);
    }

    return nullptr;
}

int main()
{
#if 0
    if (camera_capture_init() < 0)
    {
        return -1;
    }

    if (raw_frame_pool_init() < 0)
    {
        return -1;
    }

    if (camera_capture_start() < 0)
    {
        return -1;
    }

    std::cout << "Press Enter to exit..." << std::endl;
    std::cin.get();

    camera_capture_stop();
    camera_capture_cleanup();
    raw_frame_pool_cleanup();
#else
    raw_frame_pool_init();
    raw_frame_queue_init();
    if (camera_capture_init() < 0)
    {
        return -1;
    }
    
    pthread_t tid;
    pthread_create(&tid, nullptr, consumer_thread, nullptr);

    camera_capture_start();

    std::cout << "Press ENTER to exit" << std::endl;

    std::cin.get();

    camera_capture_stop();

    camera_capture_cleanup();

    raw_frame_queue_cleanup();
    raw_frame_pool_cleanup();
#endif
    return 0;
}