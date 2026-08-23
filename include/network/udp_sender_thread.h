#ifndef __UDP_SENDER_THREAD_H__
#define __UDP_SENDER_THREAD_H__

#include <cstdint>

/**
 * PIPELINE STAGE (thread wrapper, final stage): RTP Packet Queue -> [THIS] -> Network
 *
 * Consumes rtp_packet_queue and fans each packet out over UDP to every
 * destination currently registered with udp_sender (see udp_sender.h) -
 * one per PLAYING RTSP session. Returns 0 on success, -1 on failure
 * (e.g. socket() failed).
 *
 * PHASE 20 step 4: no longer takes a fixed dest_ip/dest_port - the
 * RTSP server negotiates each real client address per session (via
 * SETUP) and registers/deregisters it with udp_sender_add_dest()/
 * udp_sender_remove_dest() as sessions PLAY/TEARDOWN (see
 * rtsp_server.cpp's handle_play/handle_teardown).
 */
int udp_sender_thread_start(void);
void udp_sender_thread_stop(void);

/**
 * Cumulative packets/payload-octets successfully sent since
 * udp_sender_thread_start(). Read by rtcp_sender_thread to fill in an
 * SR's packet_count/octet_count fields.
 */
void udp_sender_thread_get_stats(uint32_t *out_packets_sent, uint32_t *out_octets_sent);

#endif // __UDP_SENDER_THREAD_H__
