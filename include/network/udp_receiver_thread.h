#ifndef __UDP_RECEIVER_THREAD_H__
#define __UDP_RECEIVER_THREAD_H__

#include <cstdint>

/**
 * PIPELINE STAGE (thread wrapper, first stage): Network -> [THIS] -> RTP Jitter Buffer
 *
 * Spawns the thread that owns the UDP socket: repeatedly receives one
 * datagram, parses its RTP header (rtp_depacketize_header), and pushes
 * the resulting rtp_packet_t into rtp_jitter_buffer for reordering.
 */

int udp_receiver_thread_start(uint16_t listen_port);
void udp_receiver_thread_stop(void);

#endif // __UDP_RECEIVER_THREAD_H__
