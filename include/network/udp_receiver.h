#ifndef __UDP_RECEIVER_H__
#define __UDP_RECEIVER_H__

#include <cstdint>
#include <cstddef>

/**
 * PIPELINE STAGE (first stage, receive side): Network -> [THIS] -> RTP Jitter Buffer
 *
 * Thin wrapper around a UDP socket for receiving RTP packets sent by
 * udp_sender (see network/udp_sender.h on the transmit side). Pure I/O,
 * no threading, no RTP parsing - used by udp_receiver_thread.
 */

/**
 * Open a UDP socket and bind it to listen_port on all local interfaces
 * (INADDR_ANY), so packets sent to this Pi's IP:listen_port are
 * received regardless of which interface they arrive on. Returns 0 on
 * success, -1 on failure.
 */
int udp_receiver_init(uint16_t listen_port);

/**
 * Blocks until one UDP datagram arrives (or the socket is closed by
 * udp_receiver_cleanup() from another thread, e.g. during shutdown, in
 * which case recvfrom() fails and this returns -1). Copies up to
 * `max_size` bytes into `buffer`. Returns the number of bytes received
 * on success, or -1 on failure/shutdown.
 */
int udp_receiver_recv(uint8_t *buffer, size_t max_size);

/**
 * Close the socket. Safe to call from a different thread than the one
 * blocked in udp_receiver_recv() - the resulting recvfrom() failure is
 * how that thread observes shutdown and exits (see udp_receiver_thread.cpp).
 */
void udp_receiver_cleanup(void);

#endif // __UDP_RECEIVER_H__
