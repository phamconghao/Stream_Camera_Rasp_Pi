#ifndef __RTP_JITTER_BUFFER_H__
#define __RTP_JITTER_BUFFER_H__

#include <cstdint>

#include "rtp_packet.h"

/**
 * ============================================================================
 * PIPELINE STAGE: UDP Receiver Thread -> [THIS] -> RTP Depacketizer Thread
 * ============================================================================
 *
 * UDP gives no ordering or delivery guarantees, so packets can arrive
 * out of order (or not at all) relative to how rtp_packetizer_thread
 * sent them. This module holds recently-arrived rtp_packet_t pointers
 * in a fixed-size ring keyed by RTP sequence number and releases them
 * to the depacketizer strictly in sequence order, so
 * rtp_depacketizer_thread never has to deal with reordering itself.
 *
 * Design (fixed-size ring, no dynamic allocation):
 *   - Slot for sequence number `seq` is `seq & (JITTER_BUFFER_CAPACITY - 1)`.
 *     Since RTP sequence numbers are 16-bit and CAPACITY is a power of
 *     two dividing 65536, this mapping is well-defined across wraparound
 *     as long as at most CAPACITY sequence numbers are "in flight" at
 *     once (true here since old slots are vacated as soon as
 *     next_expected passes them).
 *   - rtp_jitter_buffer_push() drops (releases back to the pool)
 *     packets that are duplicates or arrive too late (already passed
 *     over by next_expected) - nothing meaningful to do with them.
 *   - rtp_jitter_buffer_pop() returns the packet for next_expected as
 *     soon as it has arrived. If it hasn't arrived within
 *     JITTER_BUFFER_MAX_WAIT_MS of the *oldest* still-buffered packet
 *     that arrived after it, that sequence number is declared lost:
 *     next_expected is advanced past it (skipping the gap) and the
 *     caller is told how many sequence numbers were just skipped, via
 *     out_lost_count, so rtp_depacketizer_thread can discard whatever
 *     access unit/NAL was mid-reassembly instead of feeding the decoder
 *     a corrupt bitstream.
 *   - A big discontinuity (e.g. sender restarted, or a huge burst of
 *     loss bigger than the buffer's reorder window) is detected when an
 *     incoming packet's distance from next_expected exceeds the buffer
 *     capacity; in that case the buffer resyncs to the new packet's
 *     sequence number directly rather than waiting out the whole gap
 *     one timeout at a time.
 *
 * Thread-safety: rtp_jitter_buffer_push() is called by
 * udp_receiver_thread, rtp_jitter_buffer_pop() by
 * rtp_depacketizer_thread - both protected by an internal mutex/condvar,
 * same convention as the other queue-like modules in this project.
 */

// Must be a power of two so `seq & (CAPACITY - 1)` is a valid modulo
// operation. 64 packets is generous reorder/loss-tolerance depth for a
// LAN/loopback link at typical H.264 bitrates (a few packets per frame).
#define JITTER_BUFFER_CAPACITY 64

// How long rtp_jitter_buffer_pop() waits for a missing sequence number
// to show up before giving up on it and skipping ahead. Chosen to
// comfortably exceed normal LAN jitter while still keeping end-to-end
// latency low for a live pipeline.
#define JITTER_BUFFER_MAX_WAIT_MS 100

int rtp_jitter_buffer_init(void);
void rtp_jitter_buffer_cleanup(void);

/**
 * Insert a packet popped fresh off the socket (already stamped with
 * sequence_number by the depacketizer, see rtp_depacketizer.h). Takes
 * ownership: on a normal insert the buffer holds the pointer until
 * rtp_jitter_buffer_pop() returns it; on a duplicate/too-late/dropped
 * packet, this function releases it back to rtp_packet_pool itself and
 * the caller must not touch it again either way.
 */
void rtp_jitter_buffer_push(rtp_packet_t *packet);

/**
 * Blocks until the next in-sequence packet is ready, or until
 * rtp_jitter_buffer_shutdown() has been called and there is nothing
 * left to drain (returns nullptr in that case). *out_lost_count is set
 * to the number of sequence numbers just skipped immediately before the
 * returned packet (0 if none were lost).
 */
rtp_packet_t *rtp_jitter_buffer_pop(uint32_t *out_lost_count);

/**
 * Wake up a thread blocked in rtp_jitter_buffer_pop() so it can observe
 * shutdown and exit, same convention as *_queue_shutdown() elsewhere in
 * this project.
 */
void rtp_jitter_buffer_shutdown(void);

/**
 * Cumulative counters since rtp_jitter_buffer_init(): total packets
 * successfully accepted into the buffer, and total sequence numbers
 * declared lost (via the timeout path in rtp_jitter_buffer_pop(), same
 * count as summed out_lost_count across every pop() call). Used by
 * loss_reporter_thread to compute a loss rate for the sender's adaptive
 * bitrate control channel - see network/loss_reporter_thread.h.
 */
void rtp_jitter_buffer_get_stats(uint32_t *out_received, uint32_t *out_lost);

#endif // __RTP_JITTER_BUFFER_H__
