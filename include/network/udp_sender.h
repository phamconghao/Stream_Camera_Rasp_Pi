#ifndef __UDP_SENDER_H__
#define __UDP_SENDER_H__

#include <cstdint>
#include <cstddef>

/**
 * Thin wrapper around a UDP socket for sending pre-built RTP packets.
 * Pure I/O, no threading — used by udp_sender_thread.
 */

/**
 * Open a UDP socket and store the destination address. dest_ip must be
 * a dotted-decimal IPv4 address (e.g. "192.168.1.50"). Returns 0 on
 * success, -1 on failure.
 */
int udp_sender_init(const char *dest_ip, uint16_t dest_port);

/**
 * Send `size` bytes from `data` to the configured destination.
 * Returns the number of bytes sent, or -1 on failure.
 */
int udp_sender_send(const uint8_t *data, size_t size);

/**
 * Close the socket.
 */
void udp_sender_cleanup(void);

#endif // __UDP_SENDER_H__
