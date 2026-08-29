#include "ice_candidate.h"

#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <sstream>
#include <unordered_set>

namespace
{

// RFC 8445 section 5.1.2.2's recommended type preference table -
// this project only ever generates host and server-reflexive
// candidates so far (relay is Phase 24.4).
constexpr uint32_t kTypePreferenceHost = 126;
constexpr uint32_t kTypePreferenceServerReflexive = 100;

// RTP only - this project always uses rtcp-mux (see webrtc_sdp.cpp's
// a=rtcp-mux), so there is never a separate RTCP component.
constexpr uint32_t kComponentId = 1;

uint32_t compute_priority(uint32_t type_preference, uint32_t local_preference)
{
    // RFC 8445 section 5.1.2.1:
    //   priority = (2^24)*type_pref + (2^8)*local_pref + (2^0)*(256-component_id)
    return (type_preference << 24) | (local_preference << 8) | (256 - kComponentId);
}

} // namespace

bool ice_candidate_is_tailscale_ip(const std::string &ip_str)
{
    struct in_addr addr;
    if (inet_pton(AF_INET, ip_str.c_str(), &addr) != 1)
    {
        return false; // not a valid dotted-decimal IPv4 string
    }

    // Tailscale CGNAT range 100.64.0.0/10 (RFC 6598) = 100.64.0.0
    // through 100.127.255.255 inclusive - i.e. first octet 100,
    // second octet in [64, 127].
    uint32_t ip_host_order = ntohl(addr.s_addr);
    uint8_t first_octet = static_cast<uint8_t>((ip_host_order >> 24) & 0xFF);
    uint8_t second_octet = static_cast<uint8_t>((ip_host_order >> 16) & 0xFF);

    return first_octet == 100 && second_octet >= 64 && second_octet <= 127;
}

std::vector<ice_candidate_t> get_local_host_candidates(uint16_t port)
{
    std::vector<ice_candidate_t> candidates;

    struct ifaddrs *ifaddr = nullptr;
    if (getifaddrs(&ifaddr) < 0)
    {
        return candidates; // empty
    }

    std::unordered_set<std::string> seen_ips; // dedupe (e.g. interface aliases)

    for (struct ifaddrs *ifa = ifaddr; ifa != nullptr; ifa = ifa->ifa_next)
    {
        if (ifa->ifa_addr == nullptr || ifa->ifa_addr->sa_family != AF_INET)
        {
            continue;
        }

        // Skip loopback (127.0.0.1) - not reachable from a browser on
        // another machine, which is the only case a "candidate" is
        // even useful for (a browser on the SAME machine as camera_app
        // would work over loopback too, but that's not this project's
        // target use case).
        if (ifa->ifa_flags & IFF_LOOPBACK)
        {
            continue;
        }

        auto *addr_in = reinterpret_cast<struct sockaddr_in *>(ifa->ifa_addr);
        char ip_str[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &addr_in->sin_addr, ip_str, sizeof(ip_str));

        if (!seen_ips.insert(ip_str).second)
        {
            continue; // already have this IP from another ifaddrs entry
        }

        ice_candidate_t candidate;
        candidate.ip = ip_str;
        candidate.port = port;
        candidate.valid = true;
        candidate.is_tailscale = ice_candidate_is_tailscale_ip(ip_str);
        candidates.push_back(candidate);
    }

    freeifaddrs(ifaddr);

    // Stable-partition: non-Tailscale (LAN/other) candidates first,
    // Tailscale candidates after - see ice_candidate.h for why LAN is
    // preferred when both are reachable. Stable so relative order
    // among interfaces of the same kind (e.g. eth0 before wlan0, if
    // both happened to be up) doesn't depend on getifaddrs() ordering
    // more than it has to.
    std::stable_partition(candidates.begin(), candidates.end(),
                           [](const ice_candidate_t &c) { return !c.is_tailscale; });

    // Assign foundation + priority now that final order is known:
    // local_preference descends from 65535 (RFC 8445's max) so the
    // first (highest-priority) candidate keeps this project's
    // original fixed priority value (2130706431) for backward
    // compatibility with the single-candidate case.
    for (size_t i = 0; i < candidates.size(); ++i)
    {
        uint32_t local_preference = 65535 - static_cast<uint32_t>(i);
        candidates[i].foundation = std::to_string(i + 1);
        candidates[i].priority = compute_priority(kTypePreferenceHost, local_preference);
    }

    return candidates;
}

ice_candidate_t make_server_reflexive_candidate(
    const std::string &public_ip, uint16_t public_port,
    const std::string &base_ip, uint16_t base_port)
{
    ice_candidate_t candidate;
    candidate.ip = public_ip;
    candidate.port = public_port;
    candidate.valid = true;
    candidate.kind = ice_candidate_kind_t::SERVER_REFLEXIVE;
    candidate.related_ip = base_ip;
    candidate.related_port = base_port;
    candidate.is_tailscale = false; // a STUN-discovered public address is never a Tailscale one

    // "srflx" rather than a number - foundations only need to be a
    // valid ice-char string (RFC 8445 section 5.1.1.3), and using a
    // non-numeric one here makes collision with get_local_host_candidates()'s
    // numeric "1", "2", ... foundations structurally impossible rather
    // than just unlikely.
    candidate.foundation = "srflx";

    // Single candidate of this kind, so local_preference's exact value
    // doesn't affect ordering relative to itself - 65535 (RFC 8445's
    // max) for consistency with how host candidates are scored.
    candidate.priority = compute_priority(kTypePreferenceServerReflexive, 65535);

    return candidate;
}

std::string build_ice_candidate_line(const ice_candidate_t &candidate)
{
    if (!candidate.valid)
    {
        return "";
    }

    std::ostringstream line;
    line << "candidate:" << candidate.foundation << " " << kComponentId << " UDP " << candidate.priority
         << " " << candidate.ip << " " << candidate.port << " typ "
         << (candidate.kind == ice_candidate_kind_t::HOST ? "host" : "srflx");

    if (candidate.kind != ice_candidate_kind_t::HOST)
    {
        // raddr/rport (RFC 8839 section 5.1) - required on any
        // non-host candidate line, identifying the base address this
        // one was derived from.
        line << " raddr " << candidate.related_ip << " rport " << candidate.related_port;
    }

    return line.str();
}
