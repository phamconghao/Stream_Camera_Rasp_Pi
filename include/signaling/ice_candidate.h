#ifndef __ICE_CANDIDATE_H__
#define __ICE_CANDIDATE_H__

#include <cstdint>
#include <string>

/**
 * PHASE 22.3.1: local (host) ICE candidate generation.
 *
 * This project only ever generates a HOST candidate (the Pi's own
 * LAN IP:port, RFC 8839 section 5.1.1) - no server-reflexive (would
 * need a STUN client talking to a public STUN server) or relay
 * (would need TURN) candidates. See roadmap.md's Phase 22.3 entry for
 * why: this project targets LAN/simple-NAT use, where a host
 * candidate is normally sufficient for a direct connection, and TURN
 * in particular was a deliberate scope decision NOT to build (it's
 * infrastructure - a relay server with a public IP - not a pipeline
 * stage, and self-hosting one on the Pi itself wouldn't even solve
 * the Pi's own NAT problem).
 */

struct ice_candidate_t
{
    std::string ip;
    uint16_t port = 0;
    bool valid = false;
};

// Picks the first non-loopback IPv4 address on the machine (typically
// the Pi's single Ethernet/WiFi interface - simpler than a general
// multi-homed-host candidate gatherer would need to be) and pairs it
// with `port` (the ICE agent's UDP listen port - see ice_agent.h).
ice_candidate_t get_local_host_candidate(uint16_t port);

// Formats an ice_candidate_t as an SDP/trickle-ICE "candidate:" line
// per RFC 8839 section 5.1 - e.g.
// "candidate:1 1 UDP 2130706431 192.168.1.50 40000 typ host".
// `foundation`/`priority` are fixed constants here (this project only
// ever has exactly one candidate to offer, so there's no other
// candidate to disambiguate against or rank relative to).
std::string build_ice_candidate_line(const ice_candidate_t &candidate);

#endif // __ICE_CANDIDATE_H__
