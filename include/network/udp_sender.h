#ifndef __UDP_SENDER_H__
#define __UDP_SENDER_H__

#include <cstdint>
#include <cstddef>
#include <string>

/**
 * PIPELINE STAGE: RTP Packet Queue -> [udp_sender_thread + THIS] -> Network -> RTSP/RTP Client(s)
 *
 * Thin wrapper around a single UDP socket for sending pre-built RTP/RTCP
 * packets. Pure I/O, no threading of its own - used by udp_sender_thread
 * and (for RTCP, rtcp-mux) rtcp_sender_thread.
 *
 * PHASE 20 step 4 (RTP fan-out): one socket now fans out to a SET of
 * destinations instead of one fixed dest_ip/dest_port. Destinations are
 * keyed by RTSP session_id (rtsp_server.cpp adds one on a session's
 * READY->PLAYING transition, removes it on TEARDOWN or reap - see
 * rtsp_session_registry.h). udp_sender_send() below loops over every
 * currently-registered destination and sendto()s the same bytes to
 * each - the RTP packetizer/RTCP sender stay completely unaware that
 * there can be more than one viewer; they only ever see "one queue in,
 * one send() call out" same as before.
 */

/**
 * Open the UDP socket. No destination is known yet - callers add/remove
 * individual destinations with udp_sender_add_dest()/udp_sender_remove_dest()
 * as RTSP sessions come and go. Returns 0 on success, -1 on failure.
 */
int udp_sender_init(void);

/**
 * Register (or, if session_id already has an entry, replace) a
 * destination address that every future udp_sender_send() call fans
 * out to. dest_ip must be a dotted-decimal IPv4 address. Returns 0 on
 * success, -1 on a malformed IP.
 */
int udp_sender_add_dest(const std::string &session_id, const char *dest_ip, uint16_t dest_port);

/**
 * Unregister a previously-added destination. Safe to call for a
 * session_id that was never added (e.g. a client that TEARDOWNs
 * without ever PLAYing) - returns false in that case, true if an
 * entry was actually removed.
 */
bool udp_sender_remove_dest(const std::string &session_id);

/**
 * Number of destinations currently registered. Mainly useful for
 * logging/diagnostics.
 */
size_t udp_sender_dest_count(void);

/**
 * Send `size` bytes from `data` to every currently-registered
 * destination. Each destination is an independent sendto() - one
 * slow/unreachable client cannot block or drop packets for the
 * others. Returns the number of destinations the send() succeeded
 * for (0 if there are no destinations registered yet, e.g. pipeline
 * running momentarily between the last TEARDOWN and the next PLAY).
 */
int udp_sender_send(const uint8_t *data, size_t size);

/**
 * Close the socket and forget every registered destination.
 */
void udp_sender_cleanup(void);

#endif // __UDP_SENDER_H__
