#ifndef __ICE_CANDIDATE_H__
#define __ICE_CANDIDATE_H__

#include <cstdint>
#include <string>
#include <vector>

/**
 * Local (host), server-reflexive, and (Phase 24.4) relay ICE
 * candidate generation.
 *
 * Phase 24.2: every non-loopback IPv4 interface found is returned as
 * its own HOST candidate (RFC 8839 section 5.1.1) (typically
 * eth0/wlan0 = LAN, tailscale0 = VPN), rather than only the first one
 * - a Pi commonly has both a LAN and a Tailscale interface up
 * simultaneously, and advertising both lets the browser's ICE
 * connectivity checks pick whichever actually connects.
 *
 * Phase 24.3: adds a SERVER-REFLEXIVE candidate (RFC 8839 section
 * 5.1.1, `typ srflx`) built from the Pi's own public IP:port as
 * learned via STUN (see ice_agent.h's ice_agent_get_public_address())
 * - this is what lets a browser with no VPN and no forwarded port
 * reach the Pi at all from outside its NAT (subject to the NAT's
 * filtering behavior; see roadmap.md's Phase 24.5 for the router-side
 * forwarding that makes this reliable rather than best-effort).
 *
 * Phase 24.4: adds a RELAY candidate (RFC 8839 section 5.1.1, `typ
 * relay`) built from a TURN-allocated relay address (turn_client.h) -
 * the last-resort path for NATs where even the reflexive candidate
 * doesn't work (e.g. symmetric NAT).
 */

enum class ice_candidate_kind_t
{
    HOST,
    SERVER_REFLEXIVE,
    RELAY,
};

struct ice_candidate_t
{
    std::string ip;
    uint16_t port = 0;
    bool valid = false;

    ice_candidate_kind_t kind = ice_candidate_kind_t::HOST;

    // RFC 8839 section 5.1's raddr/rport - the base candidate's
    // address this one was derived from. Required (and only
    // meaningful) for non-HOST kinds; left empty/0 for HOST.
    std::string related_ip;
    uint16_t related_port = 0;

    // RFC 8445 section 5.1.1.3 foundation - unique per candidate here
    // since each comes from a distinct local base (interface, or the
    // STUN-derived reflexive mapping).
    std::string foundation = "1";

    // RFC 8445 section 5.1.2.1 priority, precomputed by
    // get_local_host_candidates()/make_server_reflexive_candidate() so
    // build_ice_candidate_line() stays a pure formatter. Host
    // candidates outrank the server-reflexive one (type preference
    // 126 vs 100 per RFC 8445 section 5.1.2.2's recommended table) so
    // a direct/LAN path is always preferred over the public path when
    // both are viable; LAN candidates also outrank Tailscale ones
    // among hosts (see ice_candidate.cpp).
    uint32_t priority = 2130706431;

    // Whether `ip` falls in the Tailscale CGNAT range (100.64.0.0/10,
    // RFC 6598) - see ice_candidate_is_tailscale_ip(). Always false
    // for a SERVER_REFLEXIVE candidate (a STUN-discovered public
    // address is never a Tailscale one by definition). Exposed mainly
    // for logging (main.cpp).
    bool is_tailscale = false;
};

// Enumerates every usable local HOST candidate (one per non-loopback
// IPv4 interface, deduplicated by IP), each paired with `port` (the
// ICE agent's UDP listen port - see ice_agent.h). Returned in
// priority order, highest first: non-Tailscale (LAN/other) interfaces
// before Tailscale ones. Empty vector if no usable interface exists.
std::vector<ice_candidate_t> get_local_host_candidates(uint16_t port);

// True if `ip_str` (dotted-decimal IPv4) falls within the Tailscale
// CGNAT range 100.64.0.0/10 (RFC 6598) - i.e. 100.64.0.0 through
// 100.127.255.255. A heuristic (Tailscale always assigns from this
// range, but nothing stops another CGNAT-range address from also
// matching) rather than a guarantee, which is acceptable here since
// it only affects candidate priority/labeling, never correctness -
// a misclassified candidate is still offered to the browser either
// way. Exposed (not file-local) so it can be unit tested directly.
bool ice_candidate_is_tailscale_ip(const std::string &ip_str);

// Builds a single server-reflexive candidate from a STUN-discovered
// public address (`public_ip`/`public_port`) paired with the local
// base address it was learned from (`base_ip`/`base_port` - RFC 8839
// section 5.1's raddr/rport, required for non-host candidate types).
// Caller (main.cpp) only calls this when
// ice_agent_get_public_address() reports a successful discovery.
ice_candidate_t make_server_reflexive_candidate(
    const std::string &public_ip, uint16_t public_port,
    const std::string &base_ip, uint16_t base_port);

// Builds a single relay candidate (RFC 8839 section 5.1.1, `typ
// relay`) from a TURN-allocated relay address (Phase 24.4 -
// turn_client.h's turn_client_get_relay_address()). `base_ip`/
// `base_port` here means the TURN server's own address that the
// allocation was made through, per RFC 8839 section 5.1's raddr/rport
// - NOT this project's own address the way it is for a
// server-reflexive candidate.
ice_candidate_t make_relay_candidate(
    const std::string &relay_ip, uint16_t relay_port,
    const std::string &turn_server_ip, uint16_t turn_server_port);

// Formats an ice_candidate_t as an SDP/trickle-ICE "candidate:" line
// per RFC 8839 section 5.1 - e.g.
// "candidate:1 1 UDP 2130706431 192.168.1.50 40000 typ host", or for
// a server-reflexive one, "...typ srflx raddr 192.168.1.50 rport 40000".
std::string build_ice_candidate_line(const ice_candidate_t &candidate);

#endif // __ICE_CANDIDATE_H__
