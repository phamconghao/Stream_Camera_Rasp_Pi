#include <pthread.h>
#include <stdio.h>
#include <unistd.h>
#include <iostream>

#include "encoded_frame.h"
#include "encoded_frame_queue.h"
#include "encoded_frame_pool.h"
#include "h264_writer.h"
#include "h264_nal_parser.h"

/**
 * See h264_writer.h - not started by main.cpp currently. Writes every
 * encoded access unit's raw Annex-B bytes to stream.h264 as-is (no
 * container format, just the elementary stream) - play back with e.g.
 * `ffplay stream.h264`.
 *
 * Uses the legacy std::vector-based h264_split_nals() (just for the
 * debug NAL-type printout below) rather than the iterator API that
 * rtp_packetizer_thread uses - fine here since this isn't the hot path.
 *
 * Note on shutdown: unlike the queues' blocking pop() used elsewhere,
 * this thread polls with a 1ms usleep() when the queue is empty instead
 * of blocking on the condvar - so it does NOT need
 * encoded_frame_queue_shutdown() to exit cleanly; checking
 * g_writer_running at the top of the loop is enough. This is simpler
 * but busier (wakes every 1ms even when idle) than the blocking
 * approach the other consumer threads use.
 */

static pthread_t g_thread;
static bool g_writer_running = false;
static FILE *g_fp = nullptr;

static void *writer_thread_func(void *arg)
{
    (void)arg;
    std::cout << "[WRITER] thread started" << std::endl;

    while (g_writer_running)
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

    g_writer_running = true;
    pthread_create(&g_thread, nullptr, writer_thread_func, nullptr);

    return 0;
}

void h264_writer_stop()
{
    g_writer_running = false;
    pthread_join(g_thread, nullptr);

    if (g_fp)
    {
        fclose(g_fp);
        g_fp = nullptr;
    }
}