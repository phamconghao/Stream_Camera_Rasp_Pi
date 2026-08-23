#include "ice_agent.h"

#include <pthread.h>
#include <atomic>
#include <map>
#include <cstring>
#include <cerrno>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

#include "stun_message.h"
#include "dtls_handshake.h"
#include "srtp_session.h"
#include "bcm2835_encoder.h"
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

static std::string make_addr_key(const std::string &ip, uint16_t port)
{
    return ip + ":" + std::to_string(port);
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
// STUN, DTLS, and now (Phase 22.5.4) SRTCP - the encrypted RTCP
// feedback (NACK/PLI) a browser sends back. There's no separate "RTP"
// case: this project's SDP always advertises a=sendonly (webrtc_sdp.cpp),
// meaning it never expects the browser to send it any RTP at all - any
// packet in RFC 7983's 128-191 range arriving here can only be RTCP.
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

// PHASE 22.6.6: PLI (RFC 4585 section 6.3.1) and FIR (RFC 5104
// section 4.3.1) are both explicit "I cannot decode without a fresh
// keyframe" requests from the browser - unlike generic NACK (RFC 4585
// section 6.2.1), which asks for specific lost PACKETS to be
// retransmitted, not a whole new keyframe. This project has no
// retransmission buffer to satisfy a NACK from (Phase 20's fan-out
// only ever sends forward, never replays old packets - same
// real-time-over-reliability bias documented in roadmap.md), so PLI/
// FIR are the only two feedback types this project can meaningfully
// act on: force the encoder to produce an IDR frame, exactly what
// control_listener_thread.cpp's CONTROL_MSG_KEYFRAME_REQUEST path
// already does for the RTSP/legacy control-channel case.
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
            // Phase 22.4: not STUN at all - demux by the SOURCE
            // ADDRESS this packet arrived from, per the nominated-pair
            // table populated below once that address's STUN check
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
            // Phase 22.5.4: encrypted RTCP feedback (NACK/PLI) from
            // the browser - only meaningful once that address's DTLS
            // handshake has actually completed (srtp_session_create()
            // runs at the end of dtls_handshake.cpp's handshake
            // thread, see dtls_handshake_is_connected()), so this can
            // legitimately arrive before that and should be dropped
            // quietly rather than logged as a warning - it's an
            // expected race, not a bug, and browsers do occasionally
            // send RTCP a little eagerly relative to when this
            // project's own handshake thread finishes.
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

            // Phase 22.6.6: PLI/FIR actually force a keyframe now -
            // see is_keyframe_request()'s comment for why NACK is
            // deliberately excluded (this project has nothing to
            // retransmit in response to one). This affects the SAME
            // shared hardware encoder every viewer (RTSP AND every
            // other WebRTC session) pulls frames from - one WebRTC
            // viewer's PLI benefits everyone currently watching, same
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

        stun_parsed_message_t parsed = parse_stun_message(buffer, static_cast<size_t>(n));
        if (!parsed.valid || parsed.message_type != STUN_BINDING_REQUEST)
        {
            continue; // not a well-formed STUN Binding Request - ignore silently, could be stray non-STUN UDP traffic
        }

        std::string local_pwd;
        if (!lookup_local_pwd(parsed.username, local_pwd))
        {
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
            LOG_WARN(TAG, "STUN request failed MESSAGE-INTEGRITY check (username=%s)", parsed.username.c_str());
            continue;
        }

        std::vector<uint8_t> response = build_stun_binding_response(
            parsed.transaction_id, sender_ip, sender_port, local_pwd);

        sendto(g_socket_fd, response.data(), response.size(), 0,
               reinterpret_cast<struct sockaddr *>(&sender_addr), addr_len);

        LOG_INFO(TAG, "answered STUN Binding Request from %s:%u (username=%s)",
                 sender_ip, sender_port, parsed.username.c_str());

        // Phase 22.4: this successful check nominates this pair - the
        // local ufrag identified by USERNAME's first token (see
        // lookup_local_pwd()'s comment) is now associated with this
        // exact source address for any future DTLS traffic.
        std::string local_ufrag = parsed.username.substr(0, parsed.username.find(':'));
        std::string addr_key = make_addr_key(sender_ip, sender_port);

        pthread_mutex_lock(&g_nominated_pairs_lock);
        g_nominated_pairs[addr_key] = local_ufrag;
        pthread_mutex_unlock(&g_nominated_pairs_lock);

        // Phase 22.6.2: reverse-direction map, same nomination event.
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
}
