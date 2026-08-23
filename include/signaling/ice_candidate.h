#ifndef __ICE_CANDIDATE_H__
#define __ICE_CANDIDATE_H__

#include <cstdint>
#include <string>

/**
 * Local (host) ICE candidate generation. Only ever generates a HOST
 * candidate (the device's own LAN IP:port, RFC 8839 section 5.1.1) -
 * no server-reflexive (STUN) or relay (TURN) candidates, since this
 * project targets LAN/simple-NAT use where a host candidate is
 * normally sufficient for a direct connection.
 */

struct ice_candidate_t
{
    std::string ip;
    uint16_t port = 0;
    bool valid = false;
};

// Picks the first non-loopback IPv4 address on the machine and pairs
// it with `port` (the ICE agent's UDP listen port - see ice_agent.h).
ice_candidate_t get_local_host_candidate(uint16_t port);

// Formats an ice_candidate_t as an SDP/trickle-ICE "candidate:" line
// per RFC 8839 section 5.1 - e.g.
// "candidate:1 1 UDP 2130706431 192.168.1.50 40000 typ host".
std::string build_ice_candidate_line(const ice_candidate_t &candidate);

#endif // __ICE_CANDIDATE_H__
