#include <pthread.h>
#include <stdio.h>
#include <unistd.h>
#include <iostream>

#include "encoded_frame.h"
#include "encoded_frame_queue.h"
#include "encoded_frame_pool.h"
#include "h264_writer.h"
#include "h264_nal_parser.h"

static pthread_t g_thread;
static bool g_running = false;
static FILE *g_fp = nullptr;

static void *writer_thread_func(void *arg)
{
    (void)arg;
    std::cout << "[WRITER] thread started" << std::endl;

    while (g_running)
    {
        encoded_frame_t *frame = encoded_frame_queue_pop();
        if (!frame)
        {
            usleep(1000);
            continue;
        }

        std::vector<h264_nal_t> nals;
        h264_split_nals(frame->data, frame->size, nals);
        for (auto &nal : nals)
        {
            std::cout << "[NAL] " << h264_nal_type_string(nal.nal_type) << " size = " << nal.size << std::endl;
        }

        for (int i = 0; i < 16; i++)
        {
            printf("%02X ", frame->data[i]);
        }

        printf("\n");

        fwrite(frame->data, 1, frame->size, g_fp);

        std::cout << "[WRITER] size = " << frame->size << " seq = " << frame->sequence <<std::endl;
        encoded_frame_pool_release(frame);
    }

    std::cout << "[WRITER] thread exit" << std::endl;

    return nullptr;
}

int h264_writer_start()
{
    g_fp = fopen("stream.h264", "wb");
    if (!g_fp)
    {
        perror("fopen");
        return -1;
    }

    g_running = true;
    pthread_create(&g_thread, nullptr, writer_thread_func, nullptr);

    return 0;
}

void h264_writer_stop()
{
    g_running = false;
    pthread_join(g_thread, nullptr);

    if (g_fp)
    {
        fclose(g_fp);
        g_fp = nullptr;
    }
}