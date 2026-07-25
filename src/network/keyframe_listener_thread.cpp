#include <pthread.h>
#include <cstdio>
#include <atomic>

#include "keyframe_listener_thread.h"
#include "udp_receiver.h"
#include "bcm2835_encoder.h"
#include "log.h"

static const char *TAG = "KF_LISTEN";

static pthread_t g_thread;
static std::atomic<bool> g_running(false);

// Must match keyframe_requester.cpp's KEYFRAME_REQUEST_MAGIC exactly.
static constexpr uint8_t KEYFRAME_REQUEST_MAGIC = 0xA5;

static void *keyframe_listener_thread_func(void *arg)
{
    (void)arg;

    LOG_INFO(TAG, "thread started");

    uint8_t scratch[16]; // control messages are 1 byte today; generous headroom for future extension

    while (g_running)
    {
        int n = udp_receiver_recv(scratch, sizeof(scratch));
        if (n <= 0)
        {
            // n < 0: transient recv error, or (during shutdown) the
            // socket was shut down/closed out from under us by
            // keyframe_listener_thread_stop(). n == 0: a shutdown()
            // call (see udp_receiver_cleanup()) makes a pending
            // recvfrom() return 0 rather than an error - treat the
            // same way. Either case: re-check the running flag and
            // exit cleanly if it's now false.
            continue;
        }

        if (n == 1 && scratch[0] == KEYFRAME_REQUEST_MAGIC)
        {
            LOG_INFO(TAG, "keyframe request received - forcing IDR on next frame");
            bcm2835_encoder_force_keyframe();
        }
        else
        {
            // Ignore anything else (stray traffic, port scans) rather
            // than forcing a keyframe on unrecognized input.
            LOG_WARN(TAG, "ignoring unrecognized control datagram (%d bytes)", n);
        }
    }

    LOG_INFO(TAG, "thread exit");

    return nullptr;
}

int keyframe_listener_thread_start(uint16_t control_port)
{
    if (udp_receiver_init(control_port) < 0)
    {
        return -1;
    }

    g_running = true;

    if (pthread_create(&g_thread, nullptr, keyframe_listener_thread_func, nullptr) != 0)
    {
        perror("pthread_create");
        udp_receiver_cleanup();
        g_running = false;
        return -1;
    }

    return 0;
}

void keyframe_listener_thread_stop(void)
{
    g_running = false;

    // Closing the socket is what actually unblocks a thread parked in
    // recvfrom() - same pattern as udp_receiver_thread_stop().
    udp_receiver_cleanup();

    pthread_join(g_thread, nullptr);
}
