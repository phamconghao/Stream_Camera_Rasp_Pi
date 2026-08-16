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

// RFC 7983 section 7's demultiplexing table, boiled down to just the
// two content types this project actually handles on this shared
// port (STUN and DTLS - RTP/RTCP for actual media, Phase 22.5+,
// travels over udp_sender's own separate socket, not this one, so
// there's no third case to handle here).
enum class packet_kind_t
{
    STUN,
    DTLS,
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

    return packet_kind_t::UNKNOWN;
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
