#include <pthread.h>
#include <cstring>
#include <cerrno>

#include "rtp_jitter_buffer.h"
#include "rtp_packet_pool.h"
#include "time_utils.h"
#include "log.h"

/**
 * See rtp_jitter_buffer.h for the full design rationale. This file is
 * the ring-buffer + timeout bookkeeping described there.
 */

static const char *TAG = "JITTER";

struct jitter_slot_t
{
    rtp_packet_t *packet;
    bool occupied;
    uint64_t arrival_us;
};

static jitter_slot_t g_slots[JITTER_BUFFER_CAPACITY];
static uint16_t g_next_expected;   // next sequence number pop() should release
static bool g_seq_initialized;    // false until the first packet ever seen sets g_next_expected
static int g_occupied_count;
static bool g_shutting_down;
static pthread_mutex_t g_lock;
static pthread_cond_t g_cond;

static inline int slot_index(uint16_t seq)
{
    return static_cast<int>(seq) & (JITTER_BUFFER_CAPACITY - 1);
}

// Releases and clears every occupied slot (used when a big
// discontinuity forces a full resync). Caller must hold g_lock.
static void clear_all_slots_locked()
{
    for (int i = 0; i < JITTER_BUFFER_CAPACITY; i++)
    {
        if (g_slots[i].occupied)
        {
            rtp_packet_pool_release(g_slots[i].packet);
            g_slots[i].packet = nullptr;
            g_slots[i].occupied = false;
        }
    }

    g_occupied_count = 0;
}

// Earliest arrival timestamp among all currently occupied slots, or 0
// if none are occupied (caller checks g_occupied_count first). This is
// what "how long have we been stuck waiting for next_expected" is
// measured against - if a later sequence number arrived a while ago
// and next_expected still hasn't shown up, that's our loss signal.
static uint64_t oldest_arrival_locked()
{
    uint64_t oldest = 0;
    bool found = false;

    for (int i = 0; i < JITTER_BUFFER_CAPACITY; i++)
    {
        if (g_slots[i].occupied && (!found || g_slots[i].arrival_us < oldest))
        {
            oldest = g_slots[i].arrival_us;
            found = true;
        }
    }

    return oldest;
}

int rtp_jitter_buffer_init(void)
{
    std::memset(g_slots, 0, sizeof(g_slots));
    g_next_expected = 0;
    g_seq_initialized = false;
    g_occupied_count = 0;
    g_shutting_down = false;

    pthread_mutex_init(&g_lock, nullptr);
    pthread_cond_init(&g_cond, nullptr);

    return 0;
}

void rtp_jitter_buffer_cleanup(void)
{
    pthread_mutex_lock(&g_lock);
    clear_all_slots_locked();
    pthread_mutex_unlock(&g_lock);

    pthread_mutex_destroy(&g_lock);
    pthread_cond_destroy(&g_cond);
}

void rtp_jitter_buffer_push(rtp_packet_t *packet)
{
    if (!packet)
    {
        return;
    }

    pthread_mutex_lock(&g_lock);

    if (!g_seq_initialized)
    {
        // Bootstrap: the very first packet this process ever sees
        // defines where the sequence stream starts.
        g_next_expected = packet->sequence_number;
        g_seq_initialized = true;
    }

    // Signed 16-bit circular distance from next_expected to this
    // packet's sequence number - correctly handles wraparound at 65535 -> 0.
    int16_t diff = static_cast<int16_t>(packet->sequence_number - g_next_expected);

    if (diff < 0)
    {
        // Already passed over (late duplicate, or a straggler after we
        // gave up waiting for it) - nothing useful to do with it.
        LOG_WARN(TAG, "dropping late/duplicate packet seq=%u (next_expected=%u)",
                 packet->sequence_number, g_next_expected);
        rtp_packet_pool_release(packet);
        pthread_mutex_unlock(&g_lock);
        return;
    }

    if (diff >= JITTER_BUFFER_CAPACITY)
    {
        // Discontinuity bigger than our reorder window (sender
        // restarted, or a massive loss burst) - waiting it out one
        // JITTER_BUFFER_MAX_WAIT_MS timeout at a time would take far
        // too long. Resync directly to this packet instead, counting
        // everything buffered so far as lost.
        LOG_WARN(TAG, "large sequence gap (seq=%u next_expected=%u) - resyncing",
                 packet->sequence_number, g_next_expected);
        clear_all_slots_locked();
        g_next_expected = packet->sequence_number;
    }

    int index = slot_index(packet->sequence_number);

    if (g_slots[index].occupied)
    {
        // True duplicate of a packet already buffered - keep the first
        // one, drop this one.
        LOG_WARN(TAG, "dropping duplicate packet seq=%u", packet->sequence_number);
        rtp_packet_pool_release(packet);
        pthread_mutex_unlock(&g_lock);
        return;
    }

    g_slots[index].packet = packet;
    g_slots[index].occupied = true;
    g_slots[index].arrival_us = time_utils_now_us();
    g_occupied_count++;

    pthread_cond_signal(&g_cond);
    pthread_mutex_unlock(&g_lock);
}

rtp_packet_t *rtp_jitter_buffer_pop(uint32_t *out_lost_count)
{
    uint32_t lost = 0;

    pthread_mutex_lock(&g_lock);

    for (;;)
    {
        int index = slot_index(g_next_expected);

        if (g_slots[index].occupied)
        {
            rtp_packet_t *packet = g_slots[index].packet;
            g_slots[index].packet = nullptr;
            g_slots[index].occupied = false;
            g_occupied_count--;
            g_next_expected++;

            pthread_mutex_unlock(&g_lock);

            if (out_lost_count)
            {
                *out_lost_count = lost;
            }

            return packet;
        }

        if (g_occupied_count == 0)
        {
            if (g_shutting_down)
            {
                pthread_mutex_unlock(&g_lock);
                return nullptr;
            }

            // Nothing buffered at all right now - just wait for the
            // next arrival (or shutdown) rather than spinning on a
            // timeout with nothing to measure it against.
            pthread_cond_wait(&g_cond, &g_lock);
            continue;
        }

        uint64_t oldest = oldest_arrival_locked();
        uint64_t now = time_utils_now_us();
        uint64_t elapsed_ms = (now - oldest) / 1000;

        // During shutdown, drain whatever is left immediately instead
        // of waiting out real timeouts - the sender is gone, so nothing
        // still missing is ever going to arrive.
        if (g_shutting_down || elapsed_ms >= JITTER_BUFFER_MAX_WAIT_MS)
        {
            LOG_WARN(TAG, "declaring seq=%u lost (waited %llums)",
                     g_next_expected, static_cast<unsigned long long>(elapsed_ms));
            g_next_expected++;
            lost++;
            continue;
        }

        uint64_t wait_ms = JITTER_BUFFER_MAX_WAIT_MS - elapsed_ms;

        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += static_cast<time_t>(wait_ms / 1000);
        ts.tv_nsec += static_cast<long>((wait_ms % 1000) * 1000000L);
        if (ts.tv_nsec >= 1000000000L)
        {
            ts.tv_sec += 1;
            ts.tv_nsec -= 1000000000L;
        }

        // Either a new packet arrives (cond_signal from push()) or we
        // time out and re-evaluate the loop from the top either way.
        pthread_cond_timedwait(&g_cond, &g_lock, &ts);
    }
}

void rtp_jitter_buffer_shutdown(void)
{
    pthread_mutex_lock(&g_lock);
    g_shutting_down = true;
    pthread_cond_broadcast(&g_cond);
    pthread_mutex_unlock(&g_lock);
}
