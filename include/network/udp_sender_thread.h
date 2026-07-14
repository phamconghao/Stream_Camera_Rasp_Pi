#ifndef __UDP_SENDER_THREAD_H__
#define __UDP_SENDER_THREAD_H__

#include <cstdint>

/**
 * Consumes rtp_packet_queue and sends each packet over UDP to
 * dest_ip:dest_port. Returns 0 on success, -1 on failure (e.g. bad IP).
 *
 * dest_ip/dest_port are hardcoded/passed-in for now. Once the RTSP
 * server exists, it will negotiate the real client address per session
 * (via SETUP) instead of a fixed destination.
 */
int udp_sender_thread_start(const char *dest_ip, uint16_t dest_port);
void udp_sender_thread_stop(void);

#endif // __UDP_SENDER_THREAD_H__
