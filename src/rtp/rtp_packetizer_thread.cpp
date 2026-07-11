#include <pthread.h>
#include <cstdio>
#include <atomic>

#include "encoded_frame_pool.h"
#include "encoded_frame_queue.h"
#include "rtp_packet_pool.h"
#include "rtp_packet_queue.h"
#include "rtp_packetizer.h"
#include "rtp_packetizer_thread.h"
#include "h264_nal_parser.h"
#include "log.h"

static const char *TAG = "RTP";

static pthread_t g_rtp_thread;
static uint16_t g_rtp_sequence = 0;
static uint32_t g_rtp_timestamp = 0;

static constexpr uint32_t RTP_CLOCK = 90000;
static constexpr uint32_t FRAME_RATE = 30;
static constexpr uint32_t RTP_TIMESTAMP_STEP = RTP_CLOCK / FRAME_RATE;
static constexpr uint32_t RTP_SSRC = 0x12345678;
static constexpr uint8_t RTP_PAYLOAD_TYPE_H264 = 96;

// Owned exclusively by this module — independent from app_state::g_running
// and from encoder_thread's flag, so the RTP sender can later be
// started/stopped per RTSP session without touching the encoder.
static std::atomic<bool> g_rtp_running(false);

static void *rtp_packetizer_thread_func(void *arg)
{
    (void)arg;

    LOG_INFO(TAG, "thread started");

    while (g_rtp_running)
    {
        encoded_frame_t *encoded = encoded_frame_queue_pop();
        if (!encoded)
        {
            continue;
        }

        LOG_INFO(TAG, "[ENCODED] frame seq=%u size=%zu", encoded->sequence, encoded->size);

        h264_nal_parser_t parser;
        h264_nal_parser_init(&parser, encoded->data, encoded->size);

        h264_nal_t nal;
        while (h264_nal_parser_next(&parser, &nal))
        {
            LOG_INFO(TAG, "[NAL] %s size=%zu last=%d",
                     h264_nal_type_string(nal.nal_type),
                     nal.size,
                     nal.is_last_nal ? 1 : 0);

            rtp_packet_t *packet = rtp_packet_pool_acquire();
            if (!packet)
            {
                LOG_WARN(TAG, "RTP packet pool empty");
                continue;
            }

            packet->sequence_number = g_rtp_sequence++;
            packet->timestamp = g_rtp_timestamp;
            packet->ssrc = RTP_SSRC;
            packet->payload_type = RTP_PAYLOAD_TYPE_H264;
            packet->marker = nal.is_last_nal;

            if (rtp_packetize_single_nal(&nal, packet) < 0)
            {
                LOG_WARN(TAG, "drop NAL type=%s size=%zu: FU-A not implemented yet",
                         h264_nal_type_string(nal.nal_type),
                         nal.size);
                rtp_packet_pool_release(packet);
                continue;
            }

            LOG_INFO(TAG, "[RTP] seq=%u ts=%u marker=%d payload=%zu",
                     packet->sequence_number,
                     packet->timestamp,
                     packet->marker ? 1 : 0,
                     packet->size - RTP_HEADER_SIZE);

            if (rtp_packet_queue_push(packet) < 0)
            {
                LOG_WARN(TAG, "RTP packet queue full");
                rtp_packet_pool_release(packet);
            }
        }

        g_rtp_timestamp += RTP_TIMESTAMP_STEP;

        encoded_frame_pool_release(encoded);
        LOG_INFO(TAG, "[ENCODED] frame released");
    }

    LOG_INFO(TAG, "thread exit");

    return nullptr;
}

int rtp_packetizer_thread_start(void)
{
    g_rtp_running = true;

    if (pthread_create(&g_rtp_thread, nullptr, rtp_packetizer_thread_func, nullptr) != 0)
    {
        perror("pthread_create");
        return -1;
    }

    return 0;
}

void rtp_packetizer_thread_stop(void)
{
    g_rtp_running = false;

    // Wake the thread up if it's blocked waiting for an encoded frame,
    // otherwise pthread_join below would hang forever.
    encoded_frame_queue_shutdown();

    pthread_join(g_rtp_thread, nullptr);
}
