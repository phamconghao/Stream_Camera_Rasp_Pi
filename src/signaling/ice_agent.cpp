#include "ice_agent.h"

#include <pthread.h>
#include <atomic>
#include <map>
#include <cstring>
#include <cstdlib>
#include <cerrno>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>

#include "stun_message.h"
#include "dtls_handshake.h"
#include "srtp_session.h"
#include "bcm2835_encoder.h"
#include "auth_failure_log.h"
#include "log.h"

static const char *TAG = "ICE_AGENT";

static int g_socket_fd = -1;
static pthread_t g_thread;
static std::atomic<bool> g_running(false);

static pthread_mutex_t g_sessions_lock = PTHREAD_MUTEX_INITIALIZER;
static std::map<std::string, std::string> g_sessions; // ice_ufrag -> ice_pwd

// PHASE 22.4: which ice_ufrag a given remote (ip:port) belongs to -
// populated the moment that address's STUN Binding Request first
// validates (i.e. the ICE connectivity check for that pair
// succeeded). DTLS packets carry no ufrag of their own (unlike STUN's
// USERNAME attribute), so once ICE has nominated a pair, this project
// demultiplexes DTLS traffic by SOURCE ADDRESS instead - exactly how
// a real ICE agent's post-connectivity-check traffic is normally
// identified, since by this point the 5-tuple IS the session.
static pthread_mutex_t g_nominated_pairs_lock = PTHREAD_MUTEX_INITIALIZER;
static std::map<std::string, std::string> g_nominated_pairs; // "ip:port" -> ice_ufrag

// PHASE 22.6.2: the reverse direction of the map above - populated at
// the exact same moment (a STUN check succeeding), needed so
// ice_agent_send_to_peer() can go from "which session" to "which
// socket address" without scanning g_nominated_pairs for a matching
// value every send. Deliberately a SEPARATE map rather than trying to
// keep one bidirectional structure - both directions are looked up
// far more often than they're written (once per successful STUN
// check vs. every packet demuxed/sent), so the small duplication cost
// is worth the simpler, more obviously-correct code on both sides.
static pthread_mutex_t g_ufrag_to_addr_lock = PTHREAD_MUTEX_INITIALIZER;
static std::map<std::string, struct sockaddr_in> g_ufrag_to_addr;

// PHASE 24.3: cached result of the one-time STUN self-discovery run
// inside ice_agent_start(), before the receive thread is spawned -
// see discover_public_address() below. Written exactly once (or left
// untouched on failure) before g_thread starts, so no lock is
// strictly required for later reads, but one is kept anyway since
// "written once at startup" is an invariant of the current call
// pattern, not something the type system enforces - cheap insurance
// against a future change (e.g. periodic re-discovery) silently
// introducing a real race.
static pthread_mutex_t g_public_address_lock = PTHREAD_MUTEX_INITIALIZER;
static ice_public_address_t g_public_address;
static bool g_has_public_address = false;

static std::string make_addr_key(const std::string &ip, uint16_t port)
{
    return ip + ":" + std::to_string(port);
}

// PHASE 24.3: one-time STUN self-discovery, run synchronously inside
// ice_agent_start() BEFORE the receive thread is spawned - see the
// header comment on ice_agent_get_public_address(). Deliberately
// single-threaded at the point this runs (g_running/g_thread don't
// exist yet), so it's safe to temporarily connect() the shared socket
// and block on recv() here without racing the normal receive loop.
static bool discover_public_address()
{
    const char *host_env = getenv("STUN_SERVER_HOST");
    const char *port_env = getenv("STUN_SERVER_PORT");
    std::string stun_host = (host_env != nullptr && host_env[0] != '\0') ? host_env : "stun.l.google.com";
    uint16_t stun_port = (port_env != nullptr && port_env[0] != '\0')
                              ? static_cast<uint16_t>(atoi(port_env))
                              : 19302;

    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;

    struct addrinfo *resolved = nullptr;
    std::string port_str = std::to_string(stun_port);
    int gai_err = getaddrinfo(stun_host.c_str(), port_str.c_str(), &hints, &resolved);
    if (gai_err != 0 || resolved == nullptr)
    {
        LOG_WARN(TAG, "STUN discovery: failed to resolve %s: %s", stun_host.c_str(), gai_strerror(gai_err));
        return false;
    }

    struct sockaddr_in stun_addr;
    memcpy(&stun_addr, resolved->ai_addr, sizeof(stun_addr));
    freeaddrinfo(resolved);

    // Temporarily "connect" the UDP socket to the STUN server, purely
    // so getsockname() below reports which local address the OS would
    // route this traffic out from (needed for the srflx candidate's
    // raddr - RFC 8839 section 5.1). connect() on a UDP socket only
    // fixes the default peer for send()/recv() and routing decisions -
    // no handshake occurs. Reverted (AF_UNSPEC) at the end of this
    // function regardless of outcome, since normal ICE operation must
    // keep accepting datagrams from any source address.
    if (connect(g_socket_fd, reinterpret_cast<struct sockaddr *>(&stun_addr), sizeof(stun_addr)) < 0)
    {
        LOG_WARN(TAG, "STUN discovery: connect() to %s:%u failed: %s", stun_host.c_str(), stun_port, strerror(errno));
        return false;
    }

    std::string base_ip = "0.0.0.0";
    uint16_t base_port = 0;
    struct sockaddr_in local_addr;
    socklen_t local_len = sizeof(local_addr);
    if (getsockname(g_socket_fd, reinterpret_cast<struct sockaddr *>(&local_addr), &local_len) == 0)
    {
        char ip_buf[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &local_addr.sin_addr, ip_buf, sizeof(ip_buf));
        base_ip = ip_buf;
        base_port = ntohs(local_addr.sin_port);
    }

    // Bounded wait per attempt - a public STUN server that's slow or
    // simply doesn't answer must not hang the Pi's startup
    // indefinitely (worst case here: 3 attempts * 2s = 6s added to
    // startup, then discovery is abandoned and the app proceeds
    // host-candidate-only).
    struct timeval timeout;
    timeout.tv_sec = 2;
    timeout.tv_usec = 0;
    setsockopt(g_socket_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

    bool success = false;

    for (int attempt = 0; attempt < 3 && !success; attempt++)
    {
        uint8_t transaction_id[12];
        std::vector<uint8_t> request = build_stun_binding_request_no_auth(transaction_id);

        if (send(g_socket_fd, request.data(), request.size(), 0) < 0)
        {
            LOG_WARN(TAG, "STUN discovery: send() failed (attempt %d/3): %s", attempt + 1, strerror(errno));
            continue;
        }

        uint8_t response_buf[512];
        ssize_t n = recv(g_socket_fd, response_buf, sizeof(response_buf), 0);
        if (n <= 0)
        {
            continue; // timeout (SO_RCVTIMEO) or transient error - retry
        }

        stun_parsed_message_t parsed = parse_stun_message(response_buf, static_cast<size_t>(n));
        if (!parsed.valid || parsed.message_type != STUN_BINDING_RESPONSE_SUCCESS ||
            memcmp(parsed.transaction_id, transaction_id, 12) != 0)
        {
            continue; // not the response to THIS request - stray traffic or a late retry, ignore and retry
        }

        std::string mapped_ip;
        uint16_t mapped_port = 0;
        if (!parse_stun_xor_mapped_address(response_buf, static_cast<size_t>(n), mapped_ip, mapped_port))
        {
            continue;
        }

        pthread_mutex_lock(&g_public_address_lock);
        g_public_address.public_ip = mapped_ip;
        g_public_address.public_port = mapped_port;
        g_public_address.base_ip = base_ip;
        g_public_address.base_port = base_port;
        g_has_public_address = true;
        pthread_mutex_unlock(&g_public_address_lock);

        LOG_INFO(TAG, "STUN discovery: public address is %s:%u (via %s:%u, base %s:%u)",
                 mapped_ip.c_str(), mapped_port, stun_host.c_str(), stun_port, base_ip.c_str(), base_port);

        success = true;
    }

    // Undo connect() regardless of outcome - see comment above.
    struct sockaddr unspec;
    memset(&unspec, 0, sizeof(unspec));
    unspec.sa_family = AF_UNSPEC;
    connect(g_socket_fd, &unspec, sizeof(unspec));

    // Restore blocking-forever behavior for the normal receive
    // thread's recvfrom() loop (it relies on recvfrom() only
    // returning on real data or ice_agent_stop()'s shutdown()/close(),
    // not timing out on its own).
    timeout.tv_sec = 0;
    timeout.tv_usec = 0;
    setsockopt(g_socket_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

    if (!success)
    {
        LOG_WARN(TAG, "STUN discovery: no usable response from %s:%u after 3 attempts - "
                      "no server-reflexive candidate will be offered this run",
                 stun_host.c_str(), stun_port);
    }

    return success;
}

bool ice_agent_get_public_address(ice_public_address_t &out)
{
    pthread_mutex_lock(&g_public_address_lock);
    bool has_it = g_has_public_address;
    if (has_it)
    {
        out = g_public_address;
    }
    pthread_mutex_unlock(&g_public_address_lock);
    return has_it;
}

void ice_agent_register_session(const std::string &ice_ufrag, const std::string &ice_pwd)
{
    pthread_mutex_lock(&g_sessions_lock);
    g_sessions[ice_ufrag] = ice_pwd;
    pthread_mutex_unlock(&g_sessions_lock);

    LOG_INFO(TAG, "registered session ufrag=%s", ice_ufrag.c_str());
}

void ice_agent_unregister_session(const std::string &ice_ufrag)
{
    pthread_mutex_lock(&g_sessions_lock);
    g_sessions.erase(ice_ufrag);
    pthread_mutex_unlock(&g_sessions_lock);

    // Drop any nominated-pair entries pointing at this session too -
    // not strictly required for correctness (a stale entry would just
    // fail dtls_handshake_on_packet()'s own lookup harmlessly once the
    // session is gone), but avoids the map growing unbounded across
    // many short-lived sessions.
    pthread_mutex_lock(&g_nominated_pairs_lock);
    for (auto it = g_nominated_pairs.begin(); it != g_nominated_pairs.end();)
    {
        if (it->second == ice_ufrag)
        {
            it = g_nominated_pairs.erase(it);
        }
        else
        {
            ++it;
        }
    }
    pthread_mutex_unlock(&g_nominated_pairs_lock);

    pthread_mutex_lock(&g_ufrag_to_addr_lock);
    g_ufrag_to_addr.erase(ice_ufrag);
    pthread_mutex_unlock(&g_ufrag_to_addr_lock);
}

// PHASE 22.6.2 implementation - see ice_agent.h's doc comment.
bool ice_agent_send_to_peer(const std::string &ice_ufrag, const uint8_t *data, size_t size)
{
    pthread_mutex_lock(&g_ufrag_to_addr_lock);
    auto it = g_ufrag_to_addr.find(ice_ufrag);
    if (it == g_ufrag_to_addr.end())
    {
        pthread_mutex_unlock(&g_ufrag_to_addr_lock);
        return false; // ICE hasn't nominated a pair for this session (yet, or ever)
    }
    struct sockaddr_in dest_addr = it->second;
    pthread_mutex_unlock(&g_ufrag_to_addr_lock);

    if (g_socket_fd < 0)
    {
        return false;
    }

    ssize_t sent = sendto(g_socket_fd, data, size, 0,
                           reinterpret_cast<const struct sockaddr *>(&dest_addr), sizeof(dest_addr));

    return sent >= 0;
}

// USERNAME on an incoming request is "<local_ufrag>:<remote_ufrag>"
// (RFC 8445 section 7.1.1) - the FIRST part identifies which local
// (this project's) credentials the request is addressed to, which is
// what's needed both to look up the right ice_pwd for
// stun_verify_message_integrity() and to sign the response.
static bool lookup_local_pwd(const std::string &username, std::string &out_pwd)
{
    size_t colon = username.find(':');
    if (colon == std::string::npos)
    {
        return false;
    }

    std::string local_ufrag = username.substr(0, colon);

    pthread_mutex_lock(&g_sessions_lock);
    auto it = g_sessions.find(local_ufrag);
    bool found = (it != g_sessions.end());
    if (found)
    {
        out_pwd = it->second;
    }
    pthread_mutex_unlock(&g_sessions_lock);

    return found;
}

// RFC 7983 section 7's demultiplexing table, boiled down to the
// content types this project actually handles on this shared port:
// STUN, DTLS, and SRTCP - the encrypted RTCP feedback (NACK/PLI) a
// browser sends back. There's no separate "RTP" case: this project's
// SDP always advertises a=sendonly (webrtc_sdp.cpp), meaning it never
// expects the browser to send it any RTP at all - any packet in RFC
// 7983's 128-191 range arriving here can only be RTCP.
enum class packet_kind_t
{
    STUN,
    DTLS,
    SRTCP,
    UNKNOWN,
};

static packet_kind_t classify_packet(const uint8_t *data, size_t size)
{
    if (size == 0)
    {
        return packet_kind_t::UNKNOWN;
    }

    uint8_t first_byte = data[0];

    if (first_byte <= 3)
    {
        return packet_kind_t::STUN;
    }
    if (first_byte >= 20 && first_byte <= 63)
    {
        return packet_kind_t::DTLS;
    }
    if (first_byte >= 128 && first_byte <= 191)
    {
        return packet_kind_t::SRTCP;
    }

    return packet_kind_t::UNKNOWN;
}

// Minimal RTCP feedback identification (RFC 4585 section 6.1 for
// generic NACK, RFC 4585 section 6.3 for payload-specific feedback -
// PLI is FMT=1 there) - just enough to log something a human debugging
// this project can act on, NOT a full RTCP compound-packet parser.
// Called on the PLAINTEXT bytes after srtp_session_unprotect_rtcp()
// has already removed SRTCP's encryption/auth layer.
static const char *describe_rtcp_feedback(const uint8_t *data, size_t size)
{
    if (size < 2)
    {
        return "(too short to identify)";
    }

    uint8_t fmt = data[0] & 0x1F; // low 5 bits of byte 0
    uint8_t payload_type = data[1];

    if (payload_type == 205) // RTPFB - generic RTP feedback
    {
        return (fmt == 1) ? "NACK (generic negative ack, RFC 4585)" : "RTPFB (other FMT)";
    }
    if (payload_type == 206) // PSFB - payload-specific feedback
    {
        if (fmt == 1) return "PLI (picture loss indication, RFC 4585)";
        if (fmt == 4) return "FIR (full intra request, RFC 5104)";
        return "PSFB (other FMT)";
    }

    return "(not RTPFB/PSFB - other RTCP packet type)";
}

// PLI (RFC 4585 section 6.3.1) and FIR (RFC 5104 section 4.3.1) are
// both explicit "I cannot decode without a fresh keyframe" requests
// from the browser - unlike generic NACK (RFC 4585 section 6.2.1),
// which asks for specific lost packets to be retransmitted, not a
// whole new keyframe. This project has no retransmission buffer to
// satisfy a NACK from (RTP fan-out only ever sends forward, never
// replays old packets - a real-time-over-reliability bias documented
// in roadmap.md), so PLI/FIR are the only two feedback types this
// project can meaningfully act on: force the encoder to produce an
// IDR frame, exactly what control_listener_thread.cpp's
// CONTROL_MSG_KEYFRAME_REQUEST path already does for the RTSP/legacy
// control-channel case.
static bool is_keyframe_request(const uint8_t *data, size_t size)
{
    if (size < 2)
    {
        return false;
    }

    uint8_t fmt = data[0] & 0x1F;
    uint8_t payload_type = data[1];

    return (payload_type == 206) && (fmt == 1 || fmt == 4); // PSFB + (PLI or FIR)
}

static void *ice_agent_thread_func(void *arg)
{
    (void)arg;

    LOG_INFO(TAG, "listener thread started");

    uint8_t buffer[1500]; // STUN Binding Request/Response messages are always small - well under one UDP MTU

    while (g_running)
    {
        struct sockaddr_in sender_addr;
        socklen_t addr_len = sizeof(sender_addr);

        ssize_t n = recvfrom(g_socket_fd, buffer, sizeof(buffer), 0,
                              reinterpret_cast<struct sockaddr *>(&sender_addr), &addr_len);
        if (n <= 0)
        {
            if (!g_running)
            {
                break; // socket closed by ice_agent_stop()
            }
            continue;
        }

        char sender_ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &sender_addr.sin_addr, sender_ip, sizeof(sender_ip));
        uint16_t sender_port = ntohs(sender_addr.sin_port);

        packet_kind_t kind = classify_packet(buffer, static_cast<size_t>(n));

        if (kind == packet_kind_t::DTLS)
        {
            // Not STUN at all - demux by the source address this
            // packet arrived from, per the nominated-pair table
            // populated below once that address's STUN check
            // succeeded (see the STUN success path further down).
            std::string addr_key = make_addr_key(sender_ip, sender_port);

            pthread_mutex_lock(&g_nominated_pairs_lock);
            auto pair_it = g_nominated_pairs.find(addr_key);
            bool found = (pair_it != g_nominated_pairs.end());
            std::string ufrag = found ? pair_it->second : "";
            pthread_mutex_unlock(&g_nominated_pairs_lock);

            if (!found)
            {
                LOG_WARN(TAG, "DTLS packet from unrecognized address %s:%u (no successful ICE check yet), ignoring",
                         sender_ip, sender_port);
                continue;
            }

            // send_fn captures the socket fd and this specific sender
            // address by value - dtls_handshake.cpp calls this to send
            // handshake bytes back to exactly this peer, without ever
            // needing to know this project uses sockets/sockaddr at all.
            int socket_fd = g_socket_fd;
            struct sockaddr_in dest_addr = sender_addr;
            socklen_t dest_len = addr_len;

            dtls_handshake_on_packet(ufrag, buffer, static_cast<size_t>(n),
                [socket_fd, dest_addr, dest_len](const uint8_t *data, size_t size) {
                    sendto(socket_fd, data, size, 0,
                           reinterpret_cast<const struct sockaddr *>(&dest_addr), dest_len);
                });

            continue;
        }

        if (kind == packet_kind_t::SRTCP)
        {
            // Encrypted RTCP feedback (NACK/PLI) from the browser -
            // only meaningful once that address's DTLS handshake has
            // actually completed (srtp_session_create() runs at the
            // end of dtls_handshake.cpp's handshake thread, see
            // dtls_handshake_is_connected()), so this can legitimately
            // arrive before that and should be dropped quietly rather
            // than logged as a warning - it's an expected race, not a
            // bug, and browsers do occasionally send RTCP a little
            // eagerly relative to when the handshake thread finishes.
            std::string addr_key = make_addr_key(sender_ip, sender_port);

            pthread_mutex_lock(&g_nominated_pairs_lock);
            auto pair_it = g_nominated_pairs.find(addr_key);
            bool found = (pair_it != g_nominated_pairs.end());
            std::string ufrag = found ? pair_it->second : "";
            pthread_mutex_unlock(&g_nominated_pairs_lock);

            if (!found)
            {
                continue;
            }

            int len = static_cast<int>(n);
            bool ok = srtp_session_unprotect_rtcp(ufrag, buffer, sizeof(buffer), &len);
            if (!ok)
            {
                // Either no SRTP session yet for this ufrag (handshake
                // still in progress - see comment above) or a genuine
                // auth failure (corrupted/replayed/forged packet,
                // rejected by libsrtp2's own auth tag check - see
                // srtp_session.cpp). Either way, nothing more to do
                // with it.
                continue;
            }

            LOG_INFO(TAG, "SRTCP feedback from %s:%u (ufrag=%s): %s",
                     sender_ip, sender_port, ufrag.c_str(), describe_rtcp_feedback(buffer, static_cast<size_t>(len)));

            // PLI/FIR force a keyframe - see is_keyframe_request()'s
            // comment for why NACK is deliberately excluded (this
            // project has nothing to retransmit in response to one).
            // This affects the SAME shared hardware encoder every
            // viewer (RTSP and every other WebRTC session) pulls
            // frames from - one WebRTC viewer's PLI benefits everyone
            // currently watching, same
            // as a keyframe requested via the legacy RTSP control
            // channel already does.
            if (is_keyframe_request(buffer, static_cast<size_t>(len)))
            {
                LOG_INFO(TAG, "keyframe requested via SRTCP feedback (ufrag=%s) - forcing IDR on next frame", ufrag.c_str());
                bcm2835_encoder_force_keyframe();
            }

            continue;
        }

        if (kind != packet_kind_t::STUN)
        {
            continue; // neither STUN nor DTLS - stray/malformed UDP traffic, ignore silently
        }

        // PHASE 24.3: this port is now expected to be reachable from
        // the public internet (see roadmap.md's Phase 24 direction
        // change), not just LAN/Tailscale - so, like RTSP and
        // WebSocket signaling already do, check for a standing block
        // BEFORE doing any further work on this source IP. A blocked
        // IP's packets are dropped silently here, same as any other
        // rejected STUN request (see the MESSAGE-INTEGRITY failure
        // path below) - no response either way, so a blocked attacker
        // learns nothing new by continuing to send.
        if (auth_failure_is_blocked("ICE", sender_ip))
        {
            continue;
        }

        stun_parsed_message_t parsed = parse_stun_message(buffer, static_cast<size_t>(n));
        if (!parsed.valid || parsed.message_type != STUN_BINDING_REQUEST)
        {
            continue; // not a well-formed STUN Binding Request - ignore silently, could be stray non-STUN UDP traffic
        }

        std::string local_pwd;
        if (!lookup_local_pwd(parsed.username, local_pwd))
        {
            auth_failure_log("ICE", sender_ip, "unknown ICE session (unrecognized ufrag)");
            LOG_WARN(TAG, "STUN request for unknown session (username=%s), ignoring", parsed.username.c_str());
            continue;
        }

        if (!parsed.has_message_integrity ||
            !stun_verify_message_integrity(buffer, static_cast<size_t>(n), local_pwd))
        {
            // Wrong/missing MESSAGE-INTEGRITY: either a stale
            // ice_pwd (session was re-negotiated) or - the actual
            // reason this check exists - a request that didn't
            // genuinely come from the peer this project exchanged
            // credentials with over signaling. Either way, no
            // response, since responding would help an attacker
            // probe for valid sessions.
            auth_failure_log("ICE", sender_ip, "STUN MESSAGE-INTEGRITY check failed");
            LOG_WARN(TAG, "STUN request failed MESSAGE-INTEGRITY check (username=%s)", parsed.username.c_str());
            continue;
        }

        std::vector<uint8_t> response = build_stun_binding_response(
            parsed.transaction_id, sender_ip, sender_port, local_pwd);

        sendto(g_socket_fd, response.data(), response.size(), 0,
               reinterpret_cast<struct sockaddr *>(&sender_addr), addr_len);

        LOG_INFO(TAG, "answered STUN Binding Request from %s:%u (username=%s)",
                 sender_ip, sender_port, parsed.username.c_str());

        // This successful check nominates this pair - the local ufrag
        // identified by USERNAME's first token (see lookup_local_pwd()'s
        // comment) is now associated with this exact source address
        // for any future DTLS/SRTP traffic.
        std::string local_ufrag = parsed.username.substr(0, parsed.username.find(':'));
        std::string addr_key = make_addr_key(sender_ip, sender_port);

        pthread_mutex_lock(&g_nominated_pairs_lock);
        g_nominated_pairs[addr_key] = local_ufrag;
        pthread_mutex_unlock(&g_nominated_pairs_lock);

        // Reverse-direction map, same nomination event.
        pthread_mutex_lock(&g_ufrag_to_addr_lock);
        g_ufrag_to_addr[local_ufrag] = sender_addr;
        pthread_mutex_unlock(&g_ufrag_to_addr_lock);
    }

    LOG_INFO(TAG, "listener thread exit");
    return nullptr;
}

int ice_agent_start(uint16_t port)
{
    g_socket_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (g_socket_fd < 0)
    {
        LOG_ERROR(TAG, "socket() failed: %s", strerror(errno));
        return -1;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    if (bind(g_socket_fd, reinterpret_cast<struct sockaddr *>(&addr), sizeof(addr)) < 0)
    {
        LOG_ERROR(TAG, "bind() failed on UDP port %u: %s", port, strerror(errno));
        close(g_socket_fd);
        g_socket_fd = -1;
        return -1;
    }

    // PHASE 24.3: STUN self-discovery, strictly before the receive
    // thread starts (see discover_public_address()'s comment on why
    // that ordering matters). Failure here is logged but NOT fatal to
    // startup - it just means no server-reflexive candidate gets
    // offered this run, same as if this project had never gained
    // public-internet reach at all (LAN/Tailscale host candidates from
    // Phase 24.2 are entirely unaffected).
    discover_public_address();

    g_running = true;

    if (pthread_create(&g_thread, nullptr, ice_agent_thread_func, nullptr) != 0)
    {
        LOG_ERROR(TAG, "pthread_create failed: %s", strerror(errno));
        close(g_socket_fd);
        g_socket_fd = -1;
        g_running = false;
        return -1;
    }

    LOG_INFO(TAG, "listening for STUN Binding Requests on UDP port %u", port);

    return 0;
}

void ice_agent_stop(void)
{
    g_running = false;

    if (g_socket_fd >= 0)
    {
        shutdown(g_socket_fd, SHUT_RDWR);
        close(g_socket_fd);
        g_socket_fd = -1;
    }

    pthread_join(g_thread, nullptr);

    pthread_mutex_lock(&g_sessions_lock);
    g_sessions.clear();
    pthread_mutex_unlock(&g_sessions_lock);

    pthread_mutex_lock(&g_nominated_pairs_lock);
    g_nominated_pairs.clear();
    pthread_mutex_unlock(&g_nominated_pairs_lock);

    // Reset PHASE 24.3 discovery state too, so a later restart (e.g.
    // in a test harness that calls start/stop repeatedly) doesn't
    // silently keep serving a stale mapping from a previous run.
    pthread_mutex_lock(&g_public_address_lock);
    g_public_address = ice_public_address_t();
    g_has_public_address = false;
    pthread_mutex_unlock(&g_public_address_lock);
}
