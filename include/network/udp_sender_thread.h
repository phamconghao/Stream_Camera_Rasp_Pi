#ifndef __UDP_SENDER_THREAD_H__
#define __UDP_SENDER_THREAD_H__

#include <cstdint>

/**
 * PIPELINE STAGE (thread wrapper, final stage): RTP Packet Queue -> [THIS] -> Network
 *
 * Consumes rtp_packet_queue and sends each packet over UDP to
 * dest_ip:dest_port. Returns 0 on success, -1 on failure (e.g. bad IP).
 *
 * dest_ip/dest_port are hardcoded/passed-in for now. Once the RTSP
 * server exists, it will negotiate the real client address per session
 * (via SETUP) instead of a fixed destination.
 */
int udp_sender_thread_start(const char *dest_ip, uint16_t dest_port);
void udp_sender_thread_stop(void);

/**
 * Phase 19 (RTCP): cumulative packets/payload-octets successfully sent
 * since udp_sender_thread_start(). Read by rtcp_sender_thread to fill
 * in an SR's packet_count/octet_count fields.
 */
void udp_sender_thread_get_stats(uint32_t *out_packets_sent, uint32_t *out_octets_sent);

#endif // __UDP_SENDER_THREAD_H__
