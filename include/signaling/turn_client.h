#ifndef __TURN_CLIENT_H__
#define __TURN_CLIENT_H__

#include <cstdint>
#include <cstddef>
#include <functional>
#include <string>

/**
 * PHASE 24.4: TURN (RFC 5766) client - the last-resort ICE candidate
 * type, used when neither a direct host candidate (24.2) nor a
 * server-reflexive one (24.3) lets a browser reach the Pi (e.g.
 * symmetric NAT, common on cellular carriers). Talks to a TURN server
 * (this project runs coturn - see roadmap.md's Phase 24.4 note on why
 * that's an intentional exception to this project's from-scratch
 * philosophy, the same way OpenSSL/libsrtp2 already are) over its OWN
 * dedicated UDP socket, separate from ice_agent.h's - a TURN
 * allocation is its own distinct 5-tuple, not something layered onto
 * the host/reflexive socket.
 *
 * Implements: Allocate (RFC 5766 section 6, with the long-term
 * credential challenge-response - RFC 5389 section 10.2.2), Refresh
 * (section 7, run automatically by a background thread before the
 * allocation's lifetime expires), CreatePermission (section 9, also
 * kept alive automatically for every peer this module has ever been
 * asked to permit), Send Indication (section 10.1, outbound), and
 * Data Indication (section 10.2, inbound - delivered via the callback
 * registered with turn_client_set_data_callback()).
 *
 * Deliberately NOT implemented: ChannelBind (section 11) - an
 * optimization (2-byte channel header instead of full STUN framing
 * per relayed packet) this project can add later if relay-path
 * overhead turns out to matter in practice; Send/Data indications
 * alone are fully spec-compliant and simpler to get right first.
 *
 * Scope note (see roadmap.md's Phase 24.4 entry for the full
 * picture): this module makes the relay path work for ICE
 * connectivity CHECKS (STUN over the relay, wired into
 * ice_agent.cpp) - enough for ICE to actually nominate a relay pair.
 * Routing the DTLS handshake and SRTP media themselves over an
 * already-nominated relay pair is intentionally left as follow-up
 * work, not done in this pass.
 */

struct turn_relay_address_t
{
    std::string ip;
    uint16_t port = 0;
    bool valid = false;
};

// Allocates a relay on `turn_server_host:turn_server_port` using long
// -term credentials (`username`/`password` - RFC 5389 section 10.2.2;
// coturn's simplest setup is a static `user=name:pass` entry in its
// config, which is what this project's own test setup uses). Spawns
// this module's receive thread and, on success, a background
// refresh thread that keeps both the allocation and any installed
// permissions (see turn_client_create_permission()) alive for as long
// as the process runs - callers don't need to remember to refresh
// anything themselves. Blocking; internally retries the initial
// 401-challenge round trip and transient timeouts. Returns true and
// fills out_relay on success.
bool turn_client_allocate(
    const std::string &turn_server_host, uint16_t turn_server_port,
    const std::string &username, const std::string &password,
    turn_relay_address_t &out_relay);

// Tears down the allocation (Refresh with LIFETIME=0, best-effort -
// not retried, since this is normally only called at shutdown) and
// stops both background threads. Safe to call even if
// turn_client_allocate() was never called or failed.
void turn_client_deallocate();

// True if a currently-valid allocation exists, filling out_relay the
// same as turn_client_allocate() would. Useful for main.cpp's
// per-offer candidate-building code to check without needing to hold
// onto the original turn_client_allocate() result itself.
bool turn_client_get_relay_address(turn_relay_address_t &out_relay);

// Installs (or refreshes, if already installed) a permission for
// `peer_ip` (RFC 5766 section 9 - IP-only, no port) - required before
// ANY data to or from that address will be relayed at all. The
// background refresh thread keeps every IP this has ever been called
// with alive automatically (permissions expire after 5 minutes per
// the RFC; refreshed well before that), so callers only need to call
// this once per peer, typically as soon as that peer's address is
// known (see main.cpp's handling of the browser's own "ice-candidate"
// signaling messages - a permission has to exist BEFORE the peer's
// first packet arrives, or the TURN server silently drops it and this
// project never even finds out the peer tried).
bool turn_client_create_permission(const std::string &peer_ip);

// Relays `data` to peer_ip:peer_port via a Send Indication (RFC 5766
// section 10.1, fire-and-forget - indications have no response).
// Requires a permission already installed for peer_ip (see above) -
// the TURN server drops the send otherwise, and this function has no
// way to know that happened (that's inherent to how Send Indications
// work, not a limitation of this implementation). Returns false only
// for "no allocation exists" or a local socket send() failure.
bool turn_client_send_to_peer(const std::string &peer_ip, uint16_t peer_port, const uint8_t *data, size_t size);

// Invoked (from this module's own receive thread) whenever a Data
// Indication arrives - i.e. the TURN server relayed bytes that
// peer_ip:peer_port sent to this allocation's relay address. Set once
// by ice_agent.cpp so relayed STUN traffic can be fed into the same
// classification/handling logic used for the direct/reflexive path -
// see this header's Scope note above for what's wired through this
// today versus left as follow-up.
using turn_client_data_callback_t =
    std::function<void(const std::string &peer_ip, uint16_t peer_port, const uint8_t *data, size_t size)>;
void turn_client_set_data_callback(turn_client_data_callback_t callback);

#endif // __TURN_CLIENT_H__
