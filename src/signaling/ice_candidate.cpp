#include "ice_candidate.h"

#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <cstring>
#include <sstream>

ice_candidate_t get_local_host_candidate(uint16_t port)
{
    ice_candidate_t candidate;

    struct ifaddrs *ifaddr = nullptr;
    if (getifaddrs(&ifaddr) < 0)
    {
        return candidate; // valid stays false
    }

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

        candidate.ip = ip_str;
        candidate.port = port;
        candidate.valid = true;
        break; // first non-loopback IPv4 interface - see ice_candidate.h
    }

    freeifaddrs(ifaddr);
    return candidate;
}

std::string build_ice_candidate_line(const ice_candidate_t &candidate)
{
    if (!candidate.valid)
    {
        return "";
    }

    // foundation=1, component=1 (RTP, no separate RTCP component since
    // this project always uses rtcp-mux - see webrtc_sdp.cpp's
    // a=rtcp-mux), priority is a fixed mid-range host-candidate value
    // (RFC 8445's recommended formula collapses to a constant when
    // there's only ever exactly one candidate to rank).
    std::ostringstream line;
    line << "candidate:1 1 UDP 2130706431 " << candidate.ip << " " << candidate.port << " typ host";
    return line.str();
}
