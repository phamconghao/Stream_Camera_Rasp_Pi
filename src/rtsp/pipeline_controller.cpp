#include "pipeline_controller.h"

#include <pthread.h>
#include <string>

#include "raw_frame_pool.h"
#include "raw_frame_queue.h"
#include "encoded_frame_pool.h"
#include "encoded_frame_queue.h"
#include "encoder_thread.h"
#include "bcm2835_encoder.h"
#include "camera_capture.h"
#include "rtp_packetizer_thread.h"
#include "rtp_packet_pool.h"
#include "rtp_packet_queue.h"
#include "udp_sender_thread.h"
#include "log.h"

static const char *TAG = "PIPELINE_CTRL";

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_ref_count = 0;
static std::string g_dest_ip;
static uint16_t g_dest_port = 0;

int pipeline_controller_init(const char *dest_ip, uint16_t dest_port)
{
    g_dest_ip = dest_ip;
    g_dest_port = dest_port;
    g_ref_count = 0;

    // Pools/queues + hardware device init happen once, up front - NOT
    // toggled by ensure_running()/release(). See this file's header
    // comment for why only thread start/stop is cheap enough to repeat.
    if (raw_frame_pool_init() < 0) return -1;
    if (raw_frame_queue_init() < 0) return -1;
    if (encoded_frame_pool_init() < 0) return -1;
    if (encoded_frame_queue_init() < 0) return -1;
    if (rtp_packet_pool_init() < 0) return -1;
    if (rtp_packet_queue_init() < 0) return -1;
    if (camera_capture_init() < 0) return -1;
    if (bcm2835_encoder_init(640, 480) < 0) return -1;

    LOG_INFO(TAG, "initialized (fallback dest %s:%u until fan-out lands)", dest_ip, dest_port);

    return 0;
}

void pipeline_controller_ensure_running(void)
{
    pthread_mutex_lock(&g_lock);

    int prev = g_ref_count;
    g_ref_count++;

    if (prev == 0)
    {
        LOG_INFO(TAG, "first viewer - starting pipeline");

        // Same producer-before-consumer start order main.cpp used to
        // use unconditionally: consumer threads (encoder_thread) exist
        // BEFORE the camera starts producing frames, so nothing is
        // ever pushed into a queue before its consumer thread can
        // drain it.
        encoder_thread_start();
        camera_capture_start();
        rtp_packetizer_thread_start();
        udp_sender_thread_start(g_dest_ip.c_str(), g_dest_port);
    }
    else
    {
        LOG_INFO(TAG, "pipeline already running (%d viewers now)", g_ref_count);
    }

    pthread_mutex_unlock(&g_lock);
}

void pipeline_controller_release(void)
{
    pthread_mutex_lock(&g_lock);

    if (g_ref_count == 0)
    {
        // Defensive: should be unreachable if callers only release()
        // for a session confirmed PLAYING (see this file's header
        // comment) - but don't underflow into stopping an
        // already-stopped pipeline if it somehow happens.
        LOG_WARN(TAG, "release() called with ref_count already 0 - ignoring");
        pthread_mutex_unlock(&g_lock);
        return;
    }

    g_ref_count--;

    if (g_ref_count == 0)
    {
        LOG_INFO(TAG, "last viewer left - stopping pipeline");

        // Producer-before-consumer stop order, same as main.cpp's old
        // unconditional shutdown sequence.
        camera_capture_stop();
        encoder_thread_stop();
        rtp_packetizer_thread_stop();
        udp_sender_thread_stop();
    }
    else
    {
        LOG_INFO(TAG, "pipeline still running (%d viewers left)", g_ref_count);
    }

    pthread_mutex_unlock(&g_lock);
}

void pipeline_controller_cleanup(void)
{
    pthread_mutex_lock(&g_lock);

    if (g_ref_count > 0)
    {
        // Force-stop if still running at process shutdown (e.g. app
        // exiting via Ctrl+C while RTSP clients are still connected).
        LOG_WARN(TAG, "cleanup with %d viewer(s) still active - force-stopping pipeline", g_ref_count);
        camera_capture_stop();
        encoder_thread_stop();
        rtp_packetizer_thread_stop();
        udp_sender_thread_stop();
        g_ref_count = 0;
    }

    pthread_mutex_unlock(&g_lock);

    encoded_frame_queue_cleanup();
    encoded_frame_pool_cleanup();
    rtp_packet_queue_cleanup();
    rtp_packet_pool_cleanup();
    raw_frame_queue_cleanup();
    raw_frame_pool_cleanup();
    bcm2835_encoder_cleanup();
}
