#include "turn_client.h"

#include <arpa/inet.h>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <ctime>
#include <map>
#include <mutex>
#include <netdb.h>
#include <netinet/in.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <pthread.h>
#include <random>
#include <set>
#include <sstream>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

#include "log.h"

static const char *TAG = "TURN_CLIENT";

namespace
{

// ---------------------------------------------------------------------
// RFC 5389 (STUN) + RFC 5766 (TURN) wire-format constants. A small,
// deliberate duplication of stun_message.cpp's low-level helpers
// (append_u16/u32, padding, header building) rather than sharing code
// across files - see this file's header comment / roadmap.md's Phase
// 24.4 entry for why: stun_message.cpp keeps these file-local by
// design (ICE's STUN usage and TURN's are different enough in which
// attributes/message types they need), and the helpers themselves are
// a handful of mechanical, non-security-sensitive lines - unlike the
// actual crypto (HMAC-SHA1, MD5), which this file calls through
// OpenSSL directly rather than re-deriving, same as stun_message.cpp
// does.

constexpr uint32_t STUN_MAGIC_COOKIE = 0x2112A442;
constexpr size_t STUN_HEADER_SIZE = 20;

// RFC 5766 section 13 message types (request/success/error triplets
// for the three request/response operations this client uses, plus
// the two indication types).
constexpr uint16_t TURN_MSG_ALLOCATE_REQUEST = 0x0003;
constexpr uint16_t TURN_MSG_ALLOCATE_SUCCESS = 0x0103;
constexpr uint16_t TURN_MSG_ALLOCATE_ERROR = 0x0113;
constexpr uint16_t TURN_MSG_REFRESH_REQUEST = 0x0004;
constexpr uint16_t TURN_MSG_REFRESH_SUCCESS = 0x0104;
constexpr uint16_t TURN_MSG_REFRESH_ERROR = 0x0114;
constexpr uint16_t TURN_MSG_CREATE_PERMISSION_REQUEST = 0x0008;
constexpr uint16_t TURN_MSG_CREATE_PERMISSION_SUCCESS = 0x0108;
constexpr uint16_t TURN_MSG_CREATE_PERMISSION_ERROR = 0x0118;
constexpr uint16_t TURN_MSG_SEND_INDICATION = 0x0016;
constexpr uint16_t TURN_MSG_DATA_INDICATION = 0x0017;

// RFC 5389 section 15 (base STUN attributes) + RFC 5766 section 14
// (TURN-specific attributes). Note TURN_ATTR_XOR_RELAYED_ADDRESS
// (0x0016) and TURN_MSG_SEND_INDICATION (0x0016) share a numeric
// value - harmless, since message types and attribute types are read
// from different header fields and never compared against each other.
constexpr uint16_t STUN_ATTR_USERNAME = 0x0006;
constexpr uint16_t STUN_ATTR_MESSAGE_INTEGRITY = 0x0008;
constexpr uint16_t STUN_ATTR_ERROR_CODE = 0x0009;
constexpr uint16_t STUN_ATTR_XOR_MAPPED_ADDRESS = 0x0020;
constexpr uint16_t STUN_ATTR_REALM = 0x0014;
constexpr uint16_t STUN_ATTR_NONCE = 0x0015;
constexpr uint16_t TURN_ATTR_LIFETIME = 0x000D;
constexpr uint16_t TURN_ATTR_XOR_PEER_ADDRESS = 0x0012;
constexpr uint16_t TURN_ATTR_DATA = 0x0013;
constexpr uint16_t TURN_ATTR_XOR_RELAYED_ADDRESS = 0x0016;
constexpr uint16_t TURN_ATTR_REQUESTED_TRANSPORT = 0x0019;

constexpr uint8_t TURN_TRANSPORT_UDP = 17; // IANA protocol number, RFC 5766 section 14.7

// How long before an allocation's/permission's real expiry this
// module proactively refreshes it - generous margins since a missed
// refresh silently kills the relay path with no other symptom.
constexpr uint32_t ALLOCATION_REFRESH_MARGIN_SECONDS = 60;         // refresh this many seconds before LIFETIME runs out
constexpr uint32_t DEFAULT_ALLOCATION_LIFETIME_SECONDS = 600;      // requested LIFETIME (RFC 5766 default is 600s = 10min)
constexpr uint32_t PERMISSION_LIFETIME_SECONDS = 300;              // fixed by RFC 5766 section 8 - not negotiable
constexpr uint32_t PERMISSION_REFRESH_INTERVAL_SECONDS = 240;      // refresh every 4 min, well inside the 5 min expiry

// ---------------------------------------------------------------------
// Wire-format helpers (mirrors stun_message.cpp's - see comment above).

void append_u16(std::vector<uint8_t> &out, uint16_t value)
{
    out.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
    out.push_back(static_cast<uint8_t>(value & 0xFF));
}

void append_u32(std::vector<uint8_t> &out, uint32_t value)
{
    for (int i = 3; i >= 0; i--)
    {
        out.push_back(static_cast<uint8_t>((value >> (i * 8)) & 0xFF));
    }
}

uint16_t read_u16(const uint8_t *p)
{
    return static_cast<uint16_t>((p[0] << 8) | p[1]);
}

uint32_t read_u32(const uint8_t *p)
{
    return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
}

void append_padded(std::vector<uint8_t> &out, const uint8_t *data, size_t size)
{
    out.insert(out.end(), data, data + size);
    while (out.size() % 4 != 0)
    {
        out.push_back(0);
    }
}

void append_string_attr(std::vector<uint8_t> &out, uint16_t attr_type, const std::string &value)
{
    append_u16(out, attr_type);
    append_u16(out, static_cast<uint16_t>(value.size()));
    append_padded(out, reinterpret_cast<const uint8_t *>(value.data()), value.size());
}

void patch_length(std::vector<uint8_t> &msg, size_t body_length)
{
    msg[2] = static_cast<uint8_t>((body_length >> 8) & 0xFF);
    msg[3] = static_cast<uint8_t>(body_length & 0xFF);
}

std::vector<uint8_t> message_header(uint16_t message_type, const uint8_t transaction_id[12])
{
    std::vector<uint8_t> msg;
    append_u16(msg, message_type);
    append_u16(msg, 0); // length patched in later
    append_u32(msg, STUN_MAGIC_COOKIE);
    msg.insert(msg.end(), transaction_id, transaction_id + 12);
    return msg;
}

void random_transaction_id(uint8_t out[12])
{
    std::random_device rd;
    for (int i = 0; i < 12; i++)
    {
        out[i] = static_cast<uint8_t>(rd() & 0xFF);
    }
}

// RFC 5389 section 15.4 / RFC 5766's long-term credential mechanism
// (RFC 5389 section 10.2.2): key = MD5(username ":" realm ":" password),
// used as the HMAC-SHA1 key for MESSAGE-INTEGRITY - a DIFFERENT key
// derivation than ICE's short-term mechanism (stun_message.cpp, which
// uses ice_pwd directly as the HMAC key with no hashing step), hence
// this project's two STUN-family modules not sharing a MESSAGE-INTEGRITY
// helper even though both ultimately call HMAC-SHA1.
std::vector<uint8_t> derive_long_term_key(const std::string &username, const std::string &realm, const std::string &password)
{
    std::string input = username + ":" + realm + ":" + password;

    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int digest_len = 0;

    // EVP_Digest rather than the legacy MD5() function - OpenSSL 3.0
    // deprecates the latter (this project builds with -Werror, so the
    // deprecation warning is a hard build failure) even though MD5
    // itself remains available through the EVP interface. Still MD5
    // specifically - not a design choice, RFC 5389 section 10.2.2
    // hardcodes it for this exact key derivation, and TURN servers
    // (coturn included) expect exactly this.
    EVP_Digest(input.data(), input.size(), digest, &digest_len, EVP_md5(), nullptr);

    return std::vector<uint8_t>(digest, digest + digest_len);
}

std::vector<uint8_t> hmac_sha1(const uint8_t *key, size_t key_len, const uint8_t *data, size_t size)
{
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int digest_len = 0;

    HMAC(EVP_sha1(), key, static_cast<int>(key_len), data, size, digest, &digest_len);

    return std::vector<uint8_t>(digest, digest + digest_len);
}

void append_message_integrity(std::vector<uint8_t> &msg, const std::vector<uint8_t> &key)
{
    patch_length(msg, msg.size() - STUN_HEADER_SIZE + 24);
    std::vector<uint8_t> hmac = hmac_sha1(key.data(), key.size(), msg.data(), msg.size());
    append_u16(msg, STUN_ATTR_MESSAGE_INTEGRITY);
    append_u16(msg, static_cast<uint16_t>(hmac.size()));
    msg.insert(msg.end(), hmac.begin(), hmac.end());
}

bool decode_xor_address(const uint8_t *value, uint16_t value_len, std::string &out_ip, uint16_t &out_port)
{
    if (value_len < 8)
    {
        return false;
    }

    uint8_t family = value[1];
    if (family != 0x01) // IPv4 only
    {
        return false;
    }

    uint16_t xor_port = read_u16(value + 2);
    uint32_t xor_addr = read_u32(value + 4);

    uint16_t port = static_cast<uint16_t>(xor_port ^ (STUN_MAGIC_COOKIE >> 16));
    uint32_t addr_host = xor_addr ^ STUN_MAGIC_COOKIE;

    struct in_addr addr;
    addr.s_addr = htonl(addr_host);
    char ip_buf[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &addr, ip_buf, sizeof(ip_buf));

    out_ip = ip_buf;
    out_port = port;
    return true;
}

void append_xor_address(std::vector<uint8_t> &msg, uint16_t attr_type, const std::string &ip, uint16_t port)
{
    struct in_addr addr;
    inet_pton(AF_INET, ip.c_str(), &addr);
    uint32_t addr_host = ntohl(addr.s_addr);

    uint16_t xor_port = static_cast<uint16_t>(port ^ (STUN_MAGIC_COOKIE >> 16));
    uint32_t xor_addr = addr_host ^ STUN_MAGIC_COOKIE;

    append_u16(msg, attr_type);
    append_u16(msg, 8);
    msg.push_back(0);
    msg.push_back(0x01); // IPv4
    append_u16(msg, xor_port);
    append_u32(msg, xor_addr);
}

// Generic parsed-message shape covering every response/indication type
// this client needs to read from - simpler than a separate parser per
// message type, since which fields are actually populated just
// depends on which attributes happened to be present.
struct turn_parsed_message_t
{
    bool valid = false;
    uint16_t message_type = 0;
    uint8_t transaction_id[12] = {0};

    bool has_error = false;
    int error_code = 0;

    std::string realm;
    std::string nonce;

    bool has_relayed_address = false;
    std::string relayed_ip;
    uint16_t relayed_port = 0;

    bool has_lifetime = false;
    uint32_t lifetime_seconds = 0;

    bool has_peer_address = false; // XOR-PEER-ADDRESS - on Data Indications, who sent this
    std::string peer_ip;
    uint16_t peer_port = 0;

    std::vector<uint8_t> data; // DATA attribute payload - on Data Indications
};

turn_parsed_message_t parse_turn_message(const uint8_t *data, size_t size)
{
    turn_parsed_message_t result;

    if (size < STUN_HEADER_SIZE)
    {
        return result;
    }

    result.message_type = read_u16(data);
    uint16_t body_length = read_u16(data + 2);
    uint32_t magic = read_u32(data + 4);

    if (magic != STUN_MAGIC_COOKIE)
    {
        return result;
    }

    memcpy(result.transaction_id, data + 8, 12);

    if (size < STUN_HEADER_SIZE + body_length)
    {
        return result; // truncated
    }

    size_t pos = STUN_HEADER_SIZE;
    size_t end = STUN_HEADER_SIZE + body_length;

    while (pos + 4 <= end)
    {
        uint16_t attr_type = read_u16(data + pos);
        uint16_t attr_len = read_u16(data + pos + 2);
        size_t value_start = pos + 4;

        if (value_start + attr_len > end)
        {
            break; // malformed attribute - stop, keep whatever was already found
        }

        const uint8_t *value = data + value_start;

        if (attr_type == STUN_ATTR_ERROR_CODE && attr_len >= 4)
        {
            // RFC 5389 section 15.6: byte 2 = class (top 3 bits of the
            // code, i.e. code/100), byte 3 = number (code%100).
            result.has_error = true;
            result.error_code = static_cast<int>(value[2]) * 100 + static_cast<int>(value[3]);
        }
        else if (attr_type == STUN_ATTR_REALM)
        {
            result.realm.assign(reinterpret_cast<const char *>(value), attr_len);
        }
        else if (attr_type == STUN_ATTR_NONCE)
        {
            result.nonce.assign(reinterpret_cast<const char *>(value), attr_len);
        }
        else if (attr_type == TURN_ATTR_XOR_RELAYED_ADDRESS)
        {
            result.has_relayed_address = decode_xor_address(value, attr_len, result.relayed_ip, result.relayed_port);
        }
        else if (attr_type == STUN_ATTR_XOR_MAPPED_ADDRESS)
        {
            // Present in a successful Allocate response too (this
            // project's own reflexive address, as also learned via
            // 24.3's plain STUN discovery) - not currently consumed
            // by this module (ice_agent.cpp's 24.3 discovery already
            // covers that need independently), parsed here only so
            // an unrecognized attribute doesn't trip anything up.
        }
        else if (attr_type == TURN_ATTR_LIFETIME && attr_len == 4)
        {
            result.has_lifetime = true;
            result.lifetime_seconds = read_u32(value);
        }
        else if (attr_type == TURN_ATTR_XOR_PEER_ADDRESS)
        {
            result.has_peer_address = decode_xor_address(value, attr_len, result.peer_ip, result.peer_port);
        }
        else if (attr_type == TURN_ATTR_DATA)
        {
            result.data.assign(value, value + attr_len);
        }

        size_t padded_len = (attr_len + 3) & ~static_cast<size_t>(3);
        pos = value_start + padded_len;
    }

    result.valid = true;
    return result;
}

// ---------------------------------------------------------------------
// Module state.

int g_socket_fd = -1;
struct sockaddr_in g_server_addr;
std::string g_username;
std::string g_password;

std::mutex g_auth_lock;
std::string g_realm;
std::string g_nonce;

std::mutex g_relay_lock;
turn_relay_address_t g_relay_address;
bool g_has_allocation = false;
uint32_t g_lifetime_seconds = 0;
time_t g_allocation_refresh_at = 0;

std::mutex g_peers_lock;
std::set<std::string> g_active_peers; // every IP ever passed to turn_client_create_permission()

std::mutex g_callback_lock;
turn_client_data_callback_t g_data_callback;

// Pending synchronous request/response tracking - see this file's
// header-comment-equivalent design note in turn_client.h: ONE receive
// thread owns the socket; any other thread wanting a synchronous
// exchange registers itself here, keyed by transaction ID, and waits
// on its own condition variable rather than calling recv() itself.
struct pending_request_t
{
    std::mutex mutex;
    std::condition_variable cond;
    bool completed = false;
    std::vector<uint8_t> response;
};

std::mutex g_pending_lock;
std::map<std::string, pending_request_t *> g_pending; // key: raw 12-byte transaction ID

pthread_t g_recv_thread;
pthread_t g_refresh_thread;
std::atomic<bool> g_running(false);

std::string txn_key(const uint8_t transaction_id[12])
{
    return std::string(reinterpret_cast<const char *>(transaction_id), 12);
}

// Sends `request` and blocks (with up to 3 retries, 1.5s each) for a
// response matching `transaction_id`. Any thread except g_recv_thread
// itself may call this. Returns true and fills out_response on
// success.
bool send_and_wait(const std::vector<uint8_t> &request, const uint8_t transaction_id[12],
                    std::vector<uint8_t> &out_response)
{
    if (g_socket_fd < 0)
    {
        return false;
    }

    std::string key = txn_key(transaction_id);
    pending_request_t *pending = new pending_request_t();

    {
        std::lock_guard<std::mutex> lock(g_pending_lock);
        g_pending[key] = pending;
    }

    bool ok = false;

    for (int attempt = 0; attempt < 3 && !ok; attempt++)
    {
        sendto(g_socket_fd, request.data(), request.size(), 0,
               reinterpret_cast<struct sockaddr *>(&g_server_addr), sizeof(g_server_addr));

        std::unique_lock<std::mutex> lock(pending->mutex);
        ok = pending->cond.wait_for(lock, std::chrono::milliseconds(1500), [&] { return pending->completed; });
        if (ok)
        {
            out_response = pending->response;
        }
    }

    {
        std::lock_guard<std::mutex> lock(g_pending_lock);
        g_pending.erase(key);
    }
    delete pending;

    return ok;
}

// Builds and sends one authenticated request/response exchange,
// transparently handling: the very first exchange having no
// credentials yet (triggers a 401 with REALM/NONCE - RFC 5389 section
// 10.2.2), and a stale NONCE (438 - RFC 5766 section 6.2's "server
// SHOULD accept a request... unless the NONCE is stale") by retrying
// once with the fresh NONCE the error response supplies. Same pattern
// covers Allocate, Refresh, and CreatePermission - only the
// message-specific attributes differ, supplied via `append_extra`.
bool authenticated_exchange(
    uint16_t request_message_type,
    const std::function<void(std::vector<uint8_t> &)> &append_extra,
    turn_parsed_message_t &out_response)
{
    for (int challenge_round = 0; challenge_round < 2; challenge_round++)
    {
        uint8_t transaction_id[12];
        random_transaction_id(transaction_id);

        std::vector<uint8_t> msg = message_header(request_message_type, transaction_id);
        append_extra(msg);

        std::string realm, nonce;
        {
            std::lock_guard<std::mutex> lock(g_auth_lock);
            realm = g_realm;
            nonce = g_nonce;
        }

        bool have_credentials = !realm.empty() && !nonce.empty();
        if (have_credentials)
        {
            append_string_attr(msg, STUN_ATTR_USERNAME, g_username);
            append_string_attr(msg, STUN_ATTR_REALM, realm);
            append_string_attr(msg, STUN_ATTR_NONCE, nonce);

            std::vector<uint8_t> key = derive_long_term_key(g_username, realm, g_password);
            append_message_integrity(msg, key);
        }
        else
        {
            patch_length(msg, msg.size() - STUN_HEADER_SIZE);
        }

        std::vector<uint8_t> response_bytes;
        if (!send_and_wait(msg, transaction_id, response_bytes))
        {
            LOG_WARN(TAG, "no response to message type 0x%04x (round %d/2) - server unreachable or packet loss",
                     request_message_type, challenge_round + 1);
            return false;
        }

        turn_parsed_message_t parsed = parse_turn_message(response_bytes.data(), response_bytes.size());
        if (!parsed.valid)
        {
            LOG_WARN(TAG, "malformed response to message type 0x%04x", request_message_type);
            return false;
        }

        if (parsed.has_error)
        {
            if ((parsed.error_code == 401 || parsed.error_code == 438) && challenge_round == 0)
            {
                // 401 Unauthorized (first exchange ever) or 438 Stale
                // Nonce (credentials were fine, NONCE just expired) -
                // either way the server just handed us what we need
                // to retry successfully: cache it and loop once more.
                std::lock_guard<std::mutex> lock(g_auth_lock);
                if (!parsed.realm.empty()) g_realm = parsed.realm;
                if (!parsed.nonce.empty()) g_nonce = parsed.nonce;
                continue;
            }

            LOG_WARN(TAG, "TURN error %d for message type 0x%04x", parsed.error_code, request_message_type);
            return false;
        }

        // Success - cache any fresh NONCE the response carries (some
        // servers rotate it on every response, not just on 438).
        if (!parsed.nonce.empty())
        {
            std::lock_guard<std::mutex> lock(g_auth_lock);
            g_nonce = parsed.nonce;
        }

        out_response = parsed;
        return true;
    }

    LOG_WARN(TAG, "gave up on message type 0x%04x after exhausting the auth-challenge retry budget", request_message_type);
    return false;
}

bool do_allocate(turn_relay_address_t &out_relay, uint32_t &out_lifetime)
{
    turn_parsed_message_t response;
    bool ok = authenticated_exchange(
        TURN_MSG_ALLOCATE_REQUEST,
        [](std::vector<uint8_t> &msg) {
            append_u16(msg, TURN_ATTR_REQUESTED_TRANSPORT);
            append_u16(msg, 4);
            msg.push_back(TURN_TRANSPORT_UDP);
            msg.push_back(0);
            msg.push_back(0);
            msg.push_back(0);
        },
        response);

    if (!ok || !response.has_relayed_address)
    {
        return false;
    }

    out_relay.ip = response.relayed_ip;
    out_relay.port = response.relayed_port;
    out_relay.valid = true;
    out_lifetime = response.has_lifetime ? response.lifetime_seconds : DEFAULT_ALLOCATION_LIFETIME_SECONDS;
    return true;
}

bool do_refresh(uint32_t requested_lifetime_seconds, uint32_t &out_lifetime)
{
    turn_parsed_message_t response;
    bool ok = authenticated_exchange(
        TURN_MSG_REFRESH_REQUEST,
        [requested_lifetime_seconds](std::vector<uint8_t> &msg) {
            append_u16(msg, TURN_ATTR_LIFETIME);
            append_u16(msg, 4);
            append_u32(msg, requested_lifetime_seconds);
        },
        response);

    if (!ok)
    {
        return false;
    }

    out_lifetime = response.has_lifetime ? response.lifetime_seconds : requested_lifetime_seconds;
    return true;
}

bool do_create_permission(const std::string &peer_ip)
{
    turn_parsed_message_t response;
    return authenticated_exchange(
        TURN_MSG_CREATE_PERMISSION_REQUEST,
        [&peer_ip](std::vector<uint8_t> &msg) {
            // Port doesn't matter for a permission (RFC 5766 section
            // 9 - IP-only), but XOR-PEER-ADDRESS still needs *a* port
            // value on the wire; 0 is conventional.
            append_xor_address(msg, TURN_ATTR_XOR_PEER_ADDRESS, peer_ip, 0);
        },
        response);
}

void *recv_thread_func(void *)
{
    LOG_INFO(TAG, "receive thread started");

    uint8_t buffer[2048]; // relayed DATA can carry a full RTP/DTLS packet, larger than STUN-only traffic

    while (g_running)
    {
        struct sockaddr_in from_addr;
        socklen_t from_len = sizeof(from_addr);

        ssize_t n = recvfrom(g_socket_fd, buffer, sizeof(buffer), 0,
                              reinterpret_cast<struct sockaddr *>(&from_addr), &from_len);
        if (n <= 0)
        {
            if (!g_running)
            {
                break;
            }
            continue;
        }

        turn_parsed_message_t parsed = parse_turn_message(buffer, static_cast<size_t>(n));
        if (!parsed.valid)
        {
            continue; // not a well-formed STUN/TURN message - ignore
        }

        if (parsed.message_type == TURN_MSG_DATA_INDICATION)
        {
            if (!parsed.has_peer_address || parsed.data.empty())
            {
                continue; // malformed Data Indication - missing what makes it useful
            }

            turn_client_data_callback_t callback;
            {
                std::lock_guard<std::mutex> lock(g_callback_lock);
                callback = g_data_callback;
            }

            if (callback)
            {
                callback(parsed.peer_ip, parsed.peer_port, parsed.data.data(), parsed.data.size());
            }

            continue;
        }

        // Anything else is a response to some earlier request -
        // match it to whoever's waiting by transaction ID.
        std::string key = txn_key(parsed.transaction_id);
        pending_request_t *pending = nullptr;
        {
            std::lock_guard<std::mutex> lock(g_pending_lock);
            auto it = g_pending.find(key);
            if (it != g_pending.end())
            {
                pending = it->second;
            }
        }

        if (pending != nullptr)
        {
            std::lock_guard<std::mutex> lock(pending->mutex);
            pending->response.assign(buffer, buffer + n);
            pending->completed = true;
            pending->cond.notify_one();
        }
        // else: response to a request this module already gave up
        // waiting on (timed out) - stray, nothing to do with it.
    }

    LOG_INFO(TAG, "receive thread exit");
    return nullptr;
}

void *refresh_thread_func(void *)
{
    LOG_INFO(TAG, "refresh thread started");

    while (g_running)
    {
        // Wake up once a second to check what's due - simple over
        // efficient here, since this thread does nothing but sleep
        // the overwhelming majority of the time.
        struct timespec ts = {1, 0};
        nanosleep(&ts, nullptr);

        if (!g_running)
        {
            break;
        }

        time_t now = time(nullptr);

        bool allocation_due;
        {
            std::lock_guard<std::mutex> lock(g_relay_lock);
            allocation_due = g_has_allocation && now >= g_allocation_refresh_at;
        }

        if (allocation_due)
        {
            uint32_t new_lifetime = 0;
            if (do_refresh(DEFAULT_ALLOCATION_LIFETIME_SECONDS, new_lifetime))
            {
                std::lock_guard<std::mutex> lock(g_relay_lock);
                g_lifetime_seconds = new_lifetime;
                g_allocation_refresh_at = now + static_cast<time_t>(
                    new_lifetime > ALLOCATION_REFRESH_MARGIN_SECONDS
                        ? new_lifetime - ALLOCATION_REFRESH_MARGIN_SECONDS
                        : new_lifetime / 2);
                LOG_INFO(TAG, "allocation refreshed, new lifetime %us", new_lifetime);
            }
            else
            {
                LOG_ERROR(TAG, "allocation refresh FAILED - relay candidate will stop working once the current "
                                "lifetime runs out and the TURN server drops it");
                // Deliberately not retried immediately - the next
                // second's wake-up will find allocation_due still
                // true and try again, giving transient failures room
                // to recover without a tight retry loop.
            }
        }

        // Permission refresh, independent of the allocation's own
        // schedule (they have different lifetimes - 5 min vs
        // this project's requested 10 min).
        static time_t last_permission_refresh = 0;
        if (now - last_permission_refresh >= PERMISSION_REFRESH_INTERVAL_SECONDS)
        {
            std::vector<std::string> peers;
            {
                std::lock_guard<std::mutex> lock(g_peers_lock);
                peers.assign(g_active_peers.begin(), g_active_peers.end());
            }

            for (const std::string &peer_ip : peers)
            {
                if (!do_create_permission(peer_ip))
                {
                    LOG_WARN(TAG, "permission refresh for %s failed - it may expire and stop relaying", peer_ip.c_str());
                }
            }

            last_permission_refresh = now;
        }
    }

    LOG_INFO(TAG, "refresh thread exit");
    return nullptr;
}

} // namespace

bool turn_client_allocate(
    const std::string &turn_server_host, uint16_t turn_server_port,
    const std::string &username, const std::string &password,
    turn_relay_address_t &out_relay)
{
    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;

    struct addrinfo *resolved = nullptr;
    std::string port_str = std::to_string(turn_server_port);
    int gai_err = getaddrinfo(turn_server_host.c_str(), port_str.c_str(), &hints, &resolved);
    if (gai_err != 0 || resolved == nullptr)
    {
        LOG_ERROR(TAG, "failed to resolve TURN server %s: %s", turn_server_host.c_str(), gai_strerror(gai_err));
        return false;
    }

    memcpy(&g_server_addr, resolved->ai_addr, sizeof(g_server_addr));
    freeaddrinfo(resolved);

    g_socket_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (g_socket_fd < 0)
    {
        LOG_ERROR(TAG, "socket() failed: %s", strerror(errno));
        return false;
    }

    g_username = username;
    g_password = password;
    {
        std::lock_guard<std::mutex> lock(g_auth_lock);
        g_realm.clear();
        g_nonce.clear();
    }

    g_running = true;

    if (pthread_create(&g_recv_thread, nullptr, recv_thread_func, nullptr) != 0)
    {
        LOG_ERROR(TAG, "pthread_create (recv) failed: %s", strerror(errno));
        g_running = false;
        close(g_socket_fd);
        g_socket_fd = -1;
        return false;
    }

    turn_relay_address_t relay;
    uint32_t lifetime = 0;
    bool ok = do_allocate(relay, lifetime);

    if (!ok)
    {
        LOG_ERROR(TAG, "Allocate failed against %s:%u - no relay candidate will be available this run",
                  turn_server_host.c_str(), turn_server_port);
        turn_client_deallocate();
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(g_relay_lock);
        g_relay_address = relay;
        g_has_allocation = true;
        g_lifetime_seconds = lifetime;
        g_allocation_refresh_at = time(nullptr) + static_cast<time_t>(
            lifetime > ALLOCATION_REFRESH_MARGIN_SECONDS ? lifetime - ALLOCATION_REFRESH_MARGIN_SECONDS : lifetime / 2);
    }

    if (pthread_create(&g_refresh_thread, nullptr, refresh_thread_func, nullptr) != 0)
    {
        LOG_ERROR(TAG, "pthread_create (refresh) failed: %s - allocation will NOT be kept alive automatically",
                  strerror(errno));
        // Not fatal to this call - the allocation itself succeeded,
        // it just won't self-refresh. Surfacing this loudly since
        // it's a silent-failure-later kind of problem otherwise.
    }

    out_relay = relay;
    LOG_INFO(TAG, "allocated relay %s:%u (lifetime %us) via %s:%u",
             relay.ip.c_str(), relay.port, lifetime, turn_server_host.c_str(), turn_server_port);

    return true;
}

void turn_client_deallocate()
{
    bool was_running = g_running;

    if (was_running)
    {
        // MUST happen before g_running is cleared / the socket is
        // shut down below - do_refresh() needs the receive thread
        // still alive to deliver the response back to it (see
        // send_and_wait()'s design note), and shutdown() on the
        // socket breaks further send()/recv() on it entirely. This
        // ordering bug was caught by this module's own test harness:
        // deallocation always timed out until this was fixed, because
        // the socket/thread were torn down before the LIFETIME=0
        // Refresh had any chance of getting a response.
        //
        // Best-effort - a nonresponsive server just means this never
        // gets a reply, which do_refresh()'s own retry/timeout
        // handles the same as any other unreachable-server case; not
        // worth failing this whole function over.
        uint32_t unused_lifetime = 0;
        do_refresh(0, unused_lifetime); // LIFETIME=0 explicitly deallocates, RFC 5766 section 7
    }

    g_running = false;

    if (g_socket_fd >= 0)
    {
        shutdown(g_socket_fd, SHUT_RDWR);
    }

    pthread_join(g_refresh_thread, nullptr);
    pthread_join(g_recv_thread, nullptr);

    if (g_socket_fd >= 0)
    {
        close(g_socket_fd);
        g_socket_fd = -1;
    }

    {
        std::lock_guard<std::mutex> lock(g_relay_lock);
        g_relay_address = turn_relay_address_t();
        g_has_allocation = false;
        g_lifetime_seconds = 0;
        g_allocation_refresh_at = 0;
    }

    {
        std::lock_guard<std::mutex> lock(g_peers_lock);
        g_active_peers.clear();
    }

    {
        std::lock_guard<std::mutex> lock(g_auth_lock);
        g_realm.clear();
        g_nonce.clear();
    }
}

bool turn_client_get_relay_address(turn_relay_address_t &out_relay)
{
    std::lock_guard<std::mutex> lock(g_relay_lock);
    if (!g_has_allocation)
    {
        return false;
    }
    out_relay = g_relay_address;
    return true;
}

bool turn_client_create_permission(const std::string &peer_ip)
{
    {
        std::lock_guard<std::mutex> lock(g_relay_lock);
        if (!g_has_allocation)
        {
            return false;
        }
    }

    bool ok = do_create_permission(peer_ip);
    if (ok)
    {
        std::lock_guard<std::mutex> lock(g_peers_lock);
        g_active_peers.insert(peer_ip); // tracked so the refresh thread keeps it alive - see its comment
    }
    return ok;
}

bool turn_client_send_to_peer(const std::string &peer_ip, uint16_t peer_port, const uint8_t *data, size_t size)
{
    {
        std::lock_guard<std::mutex> lock(g_relay_lock);
        if (!g_has_allocation)
        {
            return false;
        }
    }

    uint8_t transaction_id[12];
    random_transaction_id(transaction_id);

    std::vector<uint8_t> msg = message_header(TURN_MSG_SEND_INDICATION, transaction_id);
    append_xor_address(msg, TURN_ATTR_XOR_PEER_ADDRESS, peer_ip, peer_port);

    append_u16(msg, TURN_ATTR_DATA);
    append_u16(msg, static_cast<uint16_t>(size));
    append_padded(msg, data, size);

    patch_length(msg, msg.size() - STUN_HEADER_SIZE);

    ssize_t sent = sendto(g_socket_fd, msg.data(), msg.size(), 0,
                           reinterpret_cast<struct sockaddr *>(&g_server_addr), sizeof(g_server_addr));
    return sent >= 0;
}

void turn_client_set_data_callback(turn_client_data_callback_t callback)
{
    std::lock_guard<std::mutex> lock(g_callback_lock);
    g_data_callback = std::move(callback);
}
