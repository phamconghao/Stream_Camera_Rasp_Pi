#include "webrtc_sender_thread.h"

#include <pthread.h>
#include <atomic>
#include <cstring>
#include <vector>

#include <srtp2/srtp.h> // SRTP_MAX_TRAILER_LEN

#include "webrtc_rtp_packet_pool.h"
#include "webrtc_rtp_packet_queue.h"
#include "webrtc_media_registry.h"
#include "srtp_session.h"
#include "ice_agent.h"
#include "rtp_packet.h"
#include "log.h"

static const char *TAG = "WEBRTC_SEND";

static pthread_t g_thread;
static std::atomic<bool> g_running(false);

static void *webrtc_sender_thread_func(void *arg)
{
    (void)arg;

    LOG_INFO(TAG, "thread started");

    // Sized for the largest possible plaintext RTP packet PLUS SRTP's
    // auth tag/MKI trailer - srtp_protect() (via srtp_session_protect_rtp())
    // writes past the original packet length into this same buffer,
    // so it must have that headroom from the start (see
    // srtp_session.h's srtp_session_protect_rtp() doc comment).
    // Allocated ONCE outside the loop and reused every packet/session -
    // this thread only ever does one srtp_protect() + one sendto() at
    // a time (never concurrently with itself), so there's no reason
    // for a fresh buffer per iteration.
    static uint8_t scratch[RTP_MAX_PACKET_SIZE + SRTP_MAX_TRAILER_LEN];

    while (g_running)
    {
        // Blocks until rtp_packetizer_thread pushes a packet (Phase
        // 22.6.1's fan_out_to_webrtc()), or returns nullptr once
        // webrtc_rtp_packet_queue_shutdown() has been called.
        rtp_packet_t *packet = webrtc_rtp_packet_queue_pop();
        if (!packet)
        {
            continue;
        }

        std::vector<std::string> ufrags = webrtc_media_registry_get_all();

        for (const std::string &ufrag : ufrags)
        {
            // Fresh copy per viewer: srtp_protect() encrypts in
            // place, and every viewer has its own DTLS-derived keys
            // (Phase 22.4/22.5) - the plaintext `packet` itself must
            // stay untouched across iterations of this loop, or the
            // 2nd/3rd/... viewer would be protecting an already-
            // encrypted buffer instead of the original plaintext.
            memcpy(scratch, packet->data, packet->size);
            int len = static_cast<int>(packet->size);

            if (!srtp_session_protect_rtp(ufrag, scratch, sizeof(scratch), &len))
            {
                // Not necessarily an error worth alarming about: this
                // legitimately happens for the brief window between a
                // viewer being added to webrtc_media_registry (Phase
                // 22.6.3, right after SRTP session creation) - there
                // shouldn't actually be a gap there since registry
                // add happens after srtp_session_create() succeeds,
                // but a session ending (once Phase 22.6.5 wires up
                // removal) racing against this loop's snapshot of
                // ufrags is a real, expected case once that lands.
                continue;
            }

            if (!ice_agent_send_to_peer(ufrag, scratch, static_cast<size_t>(len)))
            {
                LOG_WARN(TAG, "failed to send encrypted RTP to ufrag=%s (seq=%u)",
                         ufrag.c_str(), packet->sequence_number);
            }
            else
            {
                LOG_INFO(TAG, "sent SRTP-encrypted RTP to ufrag=%s (seq=%u, plaintext=%zu -> ciphertext=%d bytes)",
                         ufrag.c_str(), packet->sequence_number, packet->size, len);
            }
        }

        webrtc_rtp_packet_pool_release(packet);
    }

    LOG_INFO(TAG, "thread exit");
    return nullptr;
}

int webrtc_sender_thread_start(void)
{
    g_running = true;

    if (pthread_create(&g_thread, nullptr, webrtc_sender_thread_func, nullptr) != 0)
    {
        LOG_ERROR(TAG, "pthread_create failed");
        g_running = false;
        return -1;
    }

    return 0;
}

void webrtc_sender_thread_stop(void)
{
    g_running = false;

    // Wake the thread up if it's blocked in webrtc_rtp_packet_queue_pop(),
    // otherwise pthread_join below would hang forever - same pattern
    // rtp_packetizer_thread_stop() uses for encoded_frame_queue.
    webrtc_rtp_packet_queue_shutdown();

    pthread_join(g_thread, nullptr);
}
