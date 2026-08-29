#ifndef __ICE_CANDIDATE_H__
#define __ICE_CANDIDATE_H__

#include <cstdint>
#include <string>
#include <vector>

/**
 * Local (host) ICE candidate generation. Only ever generates HOST
 * candidates (RFC 8839 section 5.1.1) - no server-reflexive (STUN) or
 * relay (TURN) candidates, since this project targets LAN/simple-NAT
 * (direct) and Tailscale VPN (Phase 24) use, where host candidates
 * are sufficient for a direct connection on both paths.
 *
 * Phase 24.2: every non-loopback IPv4 interface found is returned as
 * its own candidate (typically eth0/wlan0 = LAN, tailscale0 = VPN),
 * rather than only the first one - a Pi commonly has both a LAN and a
 * Tailscale interface up simultaneously, and advertising both lets
 * the browser's ICE connectivity checks pick whichever actually
 * connects: LAN when reachable (lower latency, no VPN hop), Tailscale
 * as the path that still works from a different network entirely.
 */

struct ice_candidate_t
{
    std::string ip;
    uint16_t port = 0;
    bool valid = false;

    // RFC 8445 section 5.1.1.3 foundation - unique per candidate here
    // since each comes from a distinct local base (interface).
    std::string foundation = "1";

    // RFC 8445 section 5.1.2.1 priority, precomputed by
    // get_local_host_candidates() so build_ice_candidate_line() stays
    // a pure formatter. LAN candidates are ranked above Tailscale
    // candidates (see is_tailscale in that function) so a directly
    // reachable LAN path is preferred over the VPN hop when both
    // exist, while Tailscale still lets ICE fall through to it when
    // the browser isn't on the same LAN.
    uint32_t priority = 2130706431;

    // Whether `ip` falls in the Tailscale CGNAT range (100.64.0.0/10,
    // RFC 6598) - see ice_candidate_is_tailscale_ip(). Exposed mainly
    // for logging (main.cpp) so it's obvious in logs which path a
    // given candidate is.
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

// Formats an ice_candidate_t as an SDP/trickle-ICE "candidate:" line
// per RFC 8839 section 5.1 - e.g.
// "candidate:1 1 UDP 2130706431 192.168.1.50 40000 typ host".
std::string build_ice_candidate_line(const ice_candidate_t &candidate);

#endif // __ICE_CANDIDATE_H__
