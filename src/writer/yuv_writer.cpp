#include <pthread.h>
#include <cstdio>
#include <atomic>

#include "yuv_writer.h"
#include "raw_frame_pool.h"
#include "raw_frame_queue.h"
#include "log.h"

/**
 * ============================================================================
 * PIPELINE STAGE (final stage): Raw Frame Queue -> [THIS] -> disk
 * ============================================================================
 *
 * Loop, once per decoded frame: pop from raw_frame_queue (blocks until
 * decoder_thread pushes one, same convention as udp_sender_thread on
 * the transmit side), append its raw YUV420 bytes to the output file,
 * release back to the pool.
 */

static const char *TAG = "YUV_WRITER";

static pthread_t g_writer_thread;
static std::atomic<bool> g_writer_running(false);
static FILE *g_fp = nullptr;

static void *yuv_writer_thread_func(void *arg)
{
    (void)arg;

    LOG_INFO(TAG, "thread started");

    while (g_writer_running)
    {
        // Blocks until decoder_thread pushes a frame, or returns
        // nullptr once raw_frame_queue_shutdown() has been called.
        raw_frame_t *raw = raw_frame_queue_pop();
        if (!raw)
        {
            continue;
        }

        size_t written = fwrite(raw->data, 1, raw->size, g_fp);
        if (written != raw->size)
        {
            LOG_WARN(TAG, "short write: %zu/%zu bytes (seq=%u)", written, raw->size, raw->sequence);
        }
        else
        {
            LOG_INFO(TAG, "wrote frame seq=%u size=%zu", raw->sequence, raw->size);
        }

        raw_frame_pool_release(raw);
    }

    if (g_fp)
    {
        fflush(g_fp);
    }

    LOG_INFO(TAG, "thread exit");

    return nullptr;
}

int yuv_writer_start(const char *output_path)
{
    g_fp = fopen(output_path, "wb");
    if (!g_fp)
    {
        perror("fopen");
        return -1;
    }

    g_writer_running = true;

    if (pthread_create(&g_writer_thread, nullptr, yuv_writer_thread_func, nullptr) != 0)
    {
        perror("pthread_create");
        fclose(g_fp);
        g_fp = nullptr;
        g_writer_running = false;
        return -1;
    }

    return 0;
}

void yuv_writer_stop(void)
{
    g_writer_running = false;

    // Wake the thread up if it's blocked waiting for a frame, otherwise
    // pthread_join below would hang forever once decoder_thread has
    // stopped producing frames.
    raw_frame_queue_shutdown();

    pthread_join(g_writer_thread, nullptr);

    if (g_fp)
    {
        fclose(g_fp);
        g_fp = nullptr;
    }
}
