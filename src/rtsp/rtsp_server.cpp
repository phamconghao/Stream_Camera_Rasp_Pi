#include "rtsp_server.h"

#include <pthread.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cerrno>
#include <atomic>
#include <sstream>
#include <algorithm>
#include <map>
#include <cctype>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/crypto.h>

#include "rtsp_message.h"
#include "rtsp_session_registry.h"
#include "pipeline_controller.h"
#include "udp_sender.h"
#include "sps_pps_cache.h"
#include "base64.h"
#include "auth_failure_log.h"
#include "log.h"

static const char *TAG = "RTSP_SRV";

static int g_listen_fd = -1;
static pthread_t g_accept_thread;
static pthread_t g_reaper_thread;
static std::atomic<bool> g_running(false);

static constexpr int REAPER_INTERVAL_SEC = 10;

// PHASE 23.3: RTSP Digest Authentication (RFC 2326 section 17 / RFC
// 2617) credentials - one fixed username/password pair, set once at
// startup (see rtsp_server_start()) from main.cpp's required
// RTSP_USERNAME/RTSP_PASSWORD env vars. See
// docs-security-threat-model.md section 1.3 for why Digest (not
// Basic, not a custom scheme) was chosen: every real RTSP client
// (ffplay, VLC, browsers via rtsp://user:pass@host) already speaks it,
// and it never puts the password itself on the wire.
static std::string g_rtsp_username;
static std::string g_rtsp_password;
static const char *RTSP_REALM = "rpi-camera";

// Advertised in the SETUP response's Transport header's server_port
// field (RFC 2326 section 12.39) - clients generally only use this
// for informational display / RTCP-port-guessing, not to actually
// reach the server (the client already knows our IP from the TCP
// connection it DESCRIBEd/SETUP'd over). NOT the real source port RTP
// packets go out from: udp_sender.cpp sends every session's RTP
// through ONE shared UDP socket whose source port is whatever the OS
// happened to assign it at bind time, not this fixed value - fixing
// that would mean plumbing the real ephemeral port out of
// udp_sender.cpp into here, which isn't worth it unless a real client
// is ever seen relying on server_port being accurate.
static constexpr uint16_t PLACEHOLDER_SERVER_RTP_PORT = 5004;
static constexpr uint16_t PLACEHOLDER_SERVER_RTCP_PORT = 5005;

// How long handle_describe() waits for a freshly-primed pipeline (see
// below) to produce its first SPS/PPS before giving up. 1s is generous
// for one encoded frame at any reasonable framerate/resolution this
// project targets - see pipeline_controller.h for the
// ensure_running()/release() pair this priming reuses.
static constexpr int DESCRIBE_PRIME_MAX_WAIT_ITERATIONS = 20;
static constexpr int DESCRIBE_PRIME_WAIT_STEP_MS = 50;

// The 3 bytes right after the SPS's 1-byte NAL header are
// profile_idc, constraint flags, and level_idc - exactly what RFC
// 6184 section 8.1's profile-level-id fmtp parameter wants, as 6 hex
// digits. sps here is the NAL as h264_nal_parser/sps_pps_cache hand
// it around: NAL header byte included, Annex-B start code stripped.
static std::string sps_profile_level_id_hex(const std::vector<uint8_t> &sps)
{
    static const char *HEX = "0123456789ABCDEF";

    // Defensive: an SPS with fewer than 4 bytes total is malformed and
    // should be unreachable (the encoder never emits one), but a
    // 6-hex-char placeholder keeps the SDP well-formed rather than
    // reading out of bounds if it somehow happened.
    if (sps.size() < 4)
    {
        return "000000";
    }

    std::string hex;
    hex.reserve(6);
    for (int i = 1; i <= 3; i++)
    {
        hex += HEX[(sps[i] >> 4) & 0x0F];
        hex += HEX[sps[i] & 0x0F];
    }

    return hex;
}

struct connection_ctx_t
{
    int fd;
    std::string client_ip;
};

static bool parse_client_port_range(const std::string &transport, uint16_t *out_rtp, uint16_t *out_rtcp)
{
    size_t pos = transport.find("client_port=");
    if (pos == std::string::npos)
    {
        return false;
    }

    pos += strlen("client_port=");
    int rtp = 0;
    int rtcp = 0;
    if (sscanf(transport.c_str() + pos, "%d-%d", &rtp, &rtcp) != 2)
    {
        return false;
    }

    *out_rtp = static_cast<uint16_t>(rtp);
    *out_rtcp = static_cast<uint16_t>(rtcp);
    return true;
}

/**
 * PHASE 23.3 helpers: RFC 2617 Digest Authentication.
 *
 * Deliberately the classic RFC 2069/2617 "no qop" flavor (HA1:nonce:HA2,
 * no nonce-count/cnonce) rather than qop=auth - every RTSP client this
 * project targets (ffplay, VLC, browsers via rtsp://user:pass@host)
 * supports this flavor, and the added replay protection qop=auth's
 * nonce-count gives isn't worth the extra server-side state (tracking
 * used nonce-counts per nonce) for a single-realm, single-credential,
 * LAN/VPN-only (Phase 24) server. See the nonce lifetime note on
 * connection_thread_func() below for the actual replay-protection
 * story this implementation relies on instead.
 */

static std::string md5_hex(const std::string &input)
{
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int digest_len = 0;

    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    EVP_DigestInit_ex(ctx, EVP_md5(), nullptr);
    EVP_DigestUpdate(ctx, input.data(), input.size());
    EVP_DigestFinal_ex(ctx, digest, &digest_len);
    EVP_MD_CTX_free(ctx);

    static const char *HEX = "0123456789abcdef";
    std::string hex;
    hex.reserve(digest_len * 2);
    for (unsigned int i = 0; i < digest_len; i++)
    {
        hex += HEX[(digest[i] >> 4) & 0x0F];
        hex += HEX[digest[i] & 0x0F];
    }

    return hex;
}

// 16 random bytes (OpenSSL RAND_bytes, cryptographically secure) as 32
// lowercase hex chars - one of these is generated per TCP connection
// (see connection_thread_func()) and used for every Digest challenge
// issued on that connection.
static std::string generate_nonce_hex(void)
{
    unsigned char raw[16];
    if (RAND_bytes(raw, sizeof(raw)) != 1)
    {
        // Should be unreachable on any real OpenSSL build/platform,
        // but if the CSPRNG genuinely has no entropy available, fail
        // loudly rather than silently handing out a predictable/empty
        // nonce - an attacker who can predict the nonce can precompute
        // a valid Digest response without ever seeing a real challenge.
        LOG_ERROR(TAG, "RAND_bytes failed generating RTSP Digest nonce - refusing to serve this connection");
        return "";
    }

    static const char *HEX = "0123456789abcdef";
    std::string hex;
    hex.reserve(sizeof(raw) * 2);
    for (unsigned char b : raw)
    {
        hex += HEX[(b >> 4) & 0x0F];
        hex += HEX[b & 0x0F];
    }

    return hex;
}

// Parses `Digest key1="value1", key2="value2", ...` (the part of the
// Authorization header after the "Digest " prefix) into a map. Assumes
// well-formed, unescaped quoted-string values (no embedded quotes or
// commas) - true of every real RTSP/HTTP Digest client, and a
// malformed/unparseable header just yields missing keys, which
// check_digest_auth() below already treats as a failed auth attempt.
static std::map<std::string, std::string> parse_digest_params(const std::string &s)
{
    std::map<std::string, std::string> params;
    size_t pos = 0;

    while (pos < s.size())
    {
        size_t eq = s.find('=', pos);
        if (eq == std::string::npos)
        {
            break;
        }

        std::string key = s.substr(pos, eq - pos);
        key.erase(0, key.find_first_not_of(" \t"));
        key.erase(key.find_last_not_of(" \t") + 1);

        size_t value_start = eq + 1;
        std::string value;
        if (value_start < s.size() && s[value_start] == '"')
        {
            size_t close_quote = s.find('"', value_start + 1);
            if (close_quote == std::string::npos)
            {
                break;
            }
            value = s.substr(value_start + 1, close_quote - value_start - 1);
            pos = s.find(',', close_quote);
        }
        else
        {
            size_t comma = s.find(',', value_start);
            value = s.substr(value_start, comma == std::string::npos ? std::string::npos : comma - value_start);
            pos = comma;
        }

        params[key] = value;

        if (pos == std::string::npos)
        {
            break;
        }
        pos++; // skip the comma
    }

    return params;
}

// Verifies req's Authorization header against g_rtsp_username/
// g_rtsp_password for this exact request's method+URI, and against
// `connection_nonce` - the one nonce this TCP connection's most recent
// challenge (or its first-ever challenge) handed out. A response
// computed for a different nonce (e.g. replayed from a previous,
// already-torn-down connection) is rejected, not just a wrong
// password - see connection_thread_func() for why binding to the
// connection is the relevant protection here rather than a
// server-side nonce store with expiry.
static bool check_digest_auth(const rtsp_request_t &req, const std::string &connection_nonce)
{
    std::string auth_header = rtsp_header_get(req, "Authorization");
    if (auth_header.empty())
    {
        return false;
    }

    static const std::string DIGEST_PREFIX = "Digest ";
    if (auth_header.compare(0, DIGEST_PREFIX.size(), DIGEST_PREFIX) != 0)
    {
        return false;
    }

    std::map<std::string, std::string> params = parse_digest_params(auth_header.substr(DIGEST_PREFIX.size()));

    std::string username = params["username"];
    std::string realm = params["realm"];
    std::string nonce = params["nonce"];
    std::string uri = params["uri"];
    std::string response = params["response"];

    if (username.empty() || realm.empty() || nonce.empty() || uri.empty() || response.empty())
    {
        return false;
    }

    // Username/realm/nonce aren't secrets - a plain compare (not
    // constant-time) is fine for them. Only `response` below (derived
    // from the password) needs constant-time comparison.
    if (username != g_rtsp_username || realm != RTSP_REALM || nonce != connection_nonce)
    {
        return false;
    }

    std::string ha1 = md5_hex(username + ":" + std::string(RTSP_REALM) + ":" + g_rtsp_password);
    std::string ha2 = md5_hex(req.method + ":" + uri);
    std::string expected = md5_hex(ha1 + ":" + nonce + ":" + ha2);

    std::string response_lower = response;
    std::transform(response_lower.begin(), response_lower.end(), response_lower.begin(),
                    [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    if (expected.size() != response_lower.size())
    {
        return false;
    }

    return CRYPTO_memcmp(expected.data(), response_lower.data(), expected.size()) == 0;
}

static rtsp_response_t build_unauthorized_response(const std::string &nonce)
{
    rtsp_response_t resp;
    resp.status_code = 401;
    resp.status_text = "Unauthorized";

    std::ostringstream www_authenticate;
    www_authenticate << "Digest realm=\"" << RTSP_REALM << "\", nonce=\"" << nonce << "\", algorithm=MD5";
    resp.headers["WWW-Authenticate"] = www_authenticate.str();

    return resp;
}

static rtsp_response_t handle_options(const rtsp_request_t &req)
{
    (void)req;
    rtsp_response_t resp;
    resp.status_code = 200;
    resp.status_text = "OK";
    resp.headers["Public"] = "OPTIONS, DESCRIBE, SETUP, PLAY, TEARDOWN";
    return resp;
}

static rtsp_response_t handle_describe(const rtsp_request_t &req)
{
    (void)req;
    rtsp_response_t resp;

    // SPS/PPS aren't known until the hardware encoder has actually
    // produced at least one access unit (see sps_pps_cache.h) - but
    // DESCRIBE is normally the very first request of an RTSP session,
    // sent before any SETUP/PLAY, so on a freshly-started server
    // nobody has primed the cache yet. Borrow a pipeline "viewer slot"
    // via the same ref-counted ensure_running()/release() pair
    // handle_play()/handle_teardown() use, just long enough for one
    // frame to come through. If a real viewer is concurrently PLAYing
    // (or starts PLAYing while this
    // DESCRIBE is waiting), their own ref-count entry keeps the
    // pipeline running regardless of what this call does - priming
    // never stops a pipeline someone else still needs, and releasing
    // here never stops it out from under them either.
    bool primed_pipeline = false;
    if (!sps_pps_cache_has_both())
    {
        pipeline_controller_ensure_running();
        primed_pipeline = true;

        for (int i = 0; i < DESCRIBE_PRIME_MAX_WAIT_ITERATIONS && !sps_pps_cache_has_both(); i++)
        {
            usleep(DESCRIBE_PRIME_WAIT_STEP_MS * 1000);
        }
    }

    if (!sps_pps_cache_has_both())
    {
        if (primed_pipeline)
        {
            pipeline_controller_release();
        }

        resp.status_code = 503;
        resp.status_text = "Service Unavailable";
        LOG_WARN(TAG, "DESCRIBE failed: no SPS/PPS available after priming wait (encoder unhealthy?)");
        return resp;
    }

    std::vector<uint8_t> sps = sps_pps_cache_get_sps();
    std::vector<uint8_t> pps = sps_pps_cache_get_pps();

    if (primed_pipeline)
    {
        pipeline_controller_release();
    }

    std::string profile_level_id = sps_profile_level_id_hex(sps);
    std::string sprop_parameter_sets = base64_encode(sps) + "," + base64_encode(pps);

    resp.status_code = 200;
    resp.status_text = "OK";
    resp.headers["Content-Type"] = "application/sdp";

    std::ostringstream sdp;
    sdp << "v=0\r\n";
    sdp << "o=- 0 0 IN IP4 127.0.0.1\r\n";
    sdp << "s=Raspberry Pi Camera Stream\r\n";
    sdp << "c=IN IP4 0.0.0.0\r\n";
    sdp << "t=0 0\r\n";
    sdp << "m=video 0 RTP/AVP 96\r\n";
    sdp << "a=rtpmap:96 H264/90000\r\n";
    sdp << "a=fmtp:96 packetization-mode=1;profile-level-id=" << profile_level_id
        << ";sprop-parameter-sets=" << sprop_parameter_sets << "\r\n";
    sdp << "a=control:track1\r\n";

    resp.body = sdp.str();
    return resp;
}

static rtsp_response_t handle_setup(const rtsp_request_t &req, const std::string &client_ip)
{
    rtsp_response_t resp;

    std::string transport = rtsp_header_get(req, "Transport");
    uint16_t client_rtp_port = 0;
    uint16_t client_rtcp_port = 0;

    if (!parse_client_port_range(transport, &client_rtp_port, &client_rtcp_port))
    {
        resp.status_code = 400;
        resp.status_text = "Bad Request";
        LOG_WARN(TAG, "SETUP missing/malformed Transport header: '%s'", transport.c_str());
        return resp;
    }

    std::string session_id = rtsp_session_registry_create(client_ip, client_rtp_port, client_rtcp_port);
    if (session_id.empty())
    {
        resp.status_code = 453;
        resp.status_text = "Not Enough Bandwidth";
        return resp;
    }

    resp.status_code = 200;
    resp.status_text = "OK";
    resp.headers["Session"] = session_id;

    std::ostringstream transport_resp;
    transport_resp << "RTP/AVP;unicast;client_port=" << client_rtp_port << "-" << client_rtcp_port
                    << ";server_port=" << PLACEHOLDER_SERVER_RTP_PORT << "-" << PLACEHOLDER_SERVER_RTCP_PORT;
    resp.headers["Transport"] = transport_resp.str();

    return resp;
}

static rtsp_response_t handle_play(const rtsp_request_t &req)
{
    rtsp_response_t resp;

    std::string session_id = rtsp_header_get(req, "Session");
    if (session_id.empty() || !rtsp_session_registry_exists(session_id))
    {
        resp.status_code = 454;
        resp.status_text = "Session Not Found";
        return resp;
    }

    // Only bump the pipeline's ref count (and add this session's RTP
    // destination) on a genuine READY->PLAYING transition - a client
    // re-sending PLAY on an already-playing session (some clients do
    // this to resume after a pause) must not inflate the ref count
    // beyond the number of real viewers, nor redundantly re-add a
    // destination that's already registered. See pipeline_controller.h's
    // header comment for why this matters.
    rtsp_session_state_t prev_state = rtsp_session_state_t::INIT;
    rtsp_session_registry_get_state(session_id, &prev_state);

    rtsp_session_registry_set_state(session_id, rtsp_session_state_t::PLAYING);

    if (prev_state != rtsp_session_state_t::PLAYING)
    {
        // Order matters: ensure_running() must come first. On the
        // very first viewer it's what opens udp_sender's socket (via
        // udp_sender_thread_start() -> udp_sender_init()), which
        // clears any stale destination map from a previous run -
        // adding this session's destination before that would just
        // have it wiped out immediately after.
        pipeline_controller_ensure_running();

        rtsp_session_t session;
        if (rtsp_session_registry_get(session_id, &session))
        {
            udp_sender_add_dest(session_id, session.client_ip.c_str(), session.client_rtp_port);
        }

        LOG_INFO(TAG, "session %s -> PLAYING (pipeline running, RTP dest registered)", session_id.c_str());
    }
    else
    {
        LOG_INFO(TAG, "session %s already PLAYING - PLAY is a no-op", session_id.c_str());
    }

    resp.status_code = 200;
    resp.status_text = "OK";
    resp.headers["Session"] = session_id;
    return resp;
}

static rtsp_response_t handle_teardown(const rtsp_request_t &req)
{
    rtsp_response_t resp;

    std::string session_id = rtsp_header_get(req, "Session");
    if (session_id.empty() || !rtsp_session_registry_exists(session_id))
    {
        resp.status_code = 454;
        resp.status_text = "Session Not Found";
        return resp;
    }

    // Only release the pipeline's ref count (and drop this session's
    // RTP destination) if it was actually PLAYING - a client that
    // SETUPs then immediately TEARDOWNs without ever PLAYing never
    // incremented the ref count or registered a destination in the
    // first place, so doing either here would be wrong (underflowing
    // the ref count toward stopping the pipeline for someone else who
    // legitimately is watching, in the ref-count case).
    rtsp_session_state_t prev_state = rtsp_session_state_t::INIT;
    rtsp_session_registry_get_state(session_id, &prev_state);

    rtsp_session_registry_remove(session_id);

    if (prev_state == rtsp_session_state_t::PLAYING)
    {
        udp_sender_remove_dest(session_id);
        pipeline_controller_release();
        LOG_INFO(TAG, "session %s removed (RTP dest dropped, pipeline_controller_release)", session_id.c_str());
    }
    else
    {
        LOG_INFO(TAG, "session %s removed (was never PLAYING - no dest/pipeline release needed)", session_id.c_str());
    }

    resp.status_code = 200;
    resp.status_text = "OK";
    return resp;
}

static rtsp_response_t dispatch(const rtsp_request_t &req, const std::string &client_ip, const std::string &connection_nonce)
{
    // PHASE 23.5: a client_ip already blocked for repeated auth
    // failures is short-circuited here, before even attempting to
    // verify this request's own credentials - cheaper for both sides
    // than running check_digest_auth() again just to reject it anyway.
    if (req.method != "OPTIONS" && auth_failure_is_blocked("RTSP", client_ip))
    {
        rtsp_response_t resp;
        resp.status_code = 503;
        resp.status_text = "Service Unavailable";
        resp.headers["CSeq"] = rtsp_header_get(req, "CSeq");
        return resp;
    }

    // PHASE 23.3: every method except OPTIONS requires Digest auth -
    // OPTIONS itself stays open so a client can discover the server is
    // alive and which methods it supports before it has credentials to
    // present (matches common RTSP server behavior - see
    // docs-security-threat-model.md section 1.3). TEARDOWN is
    // deliberately included even though the original Phase 23.3 plan
    // only called out SETUP/PLAY/DESCRIBE - rtsp_session_registry's
    // session IDs turned out to be an 8-hex-digit *monotonic counter*
    // (see rtsp_session_registry.cpp), not a random/unguessable token,
    // so relying on "knows the Session ID" as proof of anything would
    // let an attacker enumerate and TEARDOWN arbitrary sessions. The
    // same reasoning is why this checks auth on every single request
    // rather than the session-scoped "authenticate once at SETUP, then
    // trust the Session header" scheme the roadmap originally sketched.
    if (req.method != "OPTIONS" && !check_digest_auth(req, connection_nonce))
    {
        // PHASE 23.5 refinement: only count this toward the
        // auth_failure_log block threshold if the client actually
        // PRESENTED credentials that turned out to be wrong. A request
        // with no Authorization header at all is the normal, expected
        // first round-trip of RFC 2617's challenge/response flow -
        // every legitimate client does exactly this once per
        // connection to learn the nonce before it can compute a real
        // response. Counting that expected step as a "failure" would
        // mean every legitimate session silently spends one count
        // toward getting itself blocked for no real reason - found by
        // this phase's own end-to-end test sending only bare,
        // credential-less DESCRIBEs and hitting the block after
        // exactly AUTH_FAILURE_BLOCK_THRESHOLD (10) of them, well
        // before any actual wrong-credential attempt happened.
        if (!rtsp_header_get(req, "Authorization").empty())
        {
            auth_failure_log("RTSP", client_ip, "invalid Digest credentials");
        }

        rtsp_response_t resp = build_unauthorized_response(connection_nonce);
        resp.headers["CSeq"] = rtsp_header_get(req, "CSeq");
        return resp;
    }

    rtsp_response_t resp;

    if (req.method == "OPTIONS")
    {
        resp = handle_options(req);
    }
    else if (req.method == "DESCRIBE")
    {
        resp = handle_describe(req);
    }
    else if (req.method == "SETUP")
    {
        resp = handle_setup(req, client_ip);
    }
    else if (req.method == "PLAY")
    {
        resp = handle_play(req);
    }
    else if (req.method == "TEARDOWN")
    {
        resp = handle_teardown(req);
    }
    else
    {
        resp.status_code = 501;
        resp.status_text = "Not Implemented";
    }

    resp.headers["CSeq"] = rtsp_header_get(req, "CSeq");

    std::string session_id = rtsp_header_get(req, "Session");
    if (!session_id.empty())
    {
        rtsp_session_registry_touch(session_id);
    }

    return resp;
}

static void *connection_thread_func(void *arg)
{
    connection_ctx_t *ctx = static_cast<connection_ctx_t *>(arg);
    int fd = ctx->fd;
    std::string client_ip = ctx->client_ip;
    delete ctx;

    LOG_INFO(TAG, "connection from %s", client_ip.c_str());

    // PHASE 23.3: one Digest nonce for this connection's entire
    // lifetime, reused across every challenge/request on it (RFC
    // 2069/2617-style nonce reuse - see check_digest_auth()'s comment
    // above for why this is an accepted trade-off here rather than a
    // fresh nonce per challenge with server-side expiry tracking).
    // Binding it to the connection is what actually matters: a
    // response computed against this nonce is meaningless replayed on
    // any other connection, since that connection has - and will only
    // ever challenge with - a different, independently random nonce.
    std::string nonce = generate_nonce_hex();
    if (nonce.empty())
    {
        // generate_nonce_hex() already logged why; refuse this
        // connection entirely rather than silently falling back to
        // some fixed/predictable nonce.
        close(fd);
        return nullptr;
    }

    std::string buffer;
    char chunk[2048];

    while (g_running)
    {
        ssize_t n = recv(fd, chunk, sizeof(chunk), 0);
        if (n <= 0)
        {
            break;
        }

        buffer.append(chunk, static_cast<size_t>(n));

        size_t header_end;
        while ((header_end = buffer.find("\r\n\r\n")) != std::string::npos)
        {
            std::string request_block = buffer.substr(0, header_end + 4);
            buffer.erase(0, header_end + 4);

            rtsp_request_t req;
            if (!rtsp_parse_request(request_block, &req))
            {
                LOG_WARN(TAG, "malformed RTSP request from %s, closing connection", client_ip.c_str());
                close(fd);
                return nullptr;
            }

            LOG_INFO(TAG, "%s %s from %s", req.method.c_str(), req.uri.c_str(), client_ip.c_str());

            rtsp_response_t resp = dispatch(req, client_ip, nonce);
            std::string wire = rtsp_build_response(resp);

            ssize_t sent = send(fd, wire.c_str(), wire.size(), 0);
            if (sent < 0)
            {
                LOG_WARN(TAG, "failed to send response to %s", client_ip.c_str());
                close(fd);
                return nullptr;
            }
        }
    }

    close(fd);
    LOG_INFO(TAG, "connection from %s closed", client_ip.c_str());

    return nullptr;
}

static void *accept_thread_func(void *arg)
{
    (void)arg;

    LOG_INFO(TAG, "accept thread started");

    while (g_running)
    {
        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);

        int client_fd = accept(g_listen_fd, reinterpret_cast<struct sockaddr *>(&client_addr), &addr_len);
        if (client_fd < 0)
        {
            continue;
        }

        if (!g_running)
        {
            close(client_fd);
            break;
        }

        char ip_str[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &client_addr.sin_addr, ip_str, sizeof(ip_str));

        connection_ctx_t *ctx = new connection_ctx_t{client_fd, std::string(ip_str)};

        pthread_t conn_thread;
        if (pthread_create(&conn_thread, nullptr, connection_thread_func, ctx) != 0)
        {
            LOG_WARN(TAG, "failed to spawn connection thread for %s", ip_str);
            delete ctx;
            close(client_fd);
            continue;
        }

        pthread_detach(conn_thread);
    }

    LOG_INFO(TAG, "accept thread exit");

    return nullptr;
}

static void *reaper_thread_func(void *arg)
{
    (void)arg;

    LOG_INFO(TAG, "reaper thread started");

    int waited_sec = 0;

    while (g_running)
    {
        sleep(1);
        waited_sec++;

        if (waited_sec < REAPER_INTERVAL_SEC)
        {
            continue;
        }

        waited_sec = 0;

        std::vector<rtsp_session_t> reaped = rtsp_session_registry_reap_orphans();
        for (const auto &session : reaped)
        {
            if (session.state == rtsp_session_state_t::PLAYING)
            {
                // This session was actively streaming when it went
                // silent (crashed client, network drop with no
                // TEARDOWN ever received) - without this, udp_sender
                // would keep fanning packets out to a dead client's
                // address forever, and the pipeline would keep running
                // forever for a viewer who's gone, since nothing else
                // would ever call udp_sender_remove_dest()/release()
                // for it.
                udp_sender_remove_dest(session.session_id);
                pipeline_controller_release();
                LOG_WARN(TAG, "dropped RTP dest + released pipeline ref for reaped PLAYING session %s",
                         session.session_id.c_str());
            }
        }
    }

    LOG_INFO(TAG, "reaper thread exit");

    return nullptr;
}

int rtsp_server_start(uint16_t port, const std::string &username, const std::string &password)
{
    g_rtsp_username = username;
    g_rtsp_password = password;

    g_listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (g_listen_fd < 0)
    {
        LOG_ERROR(TAG, "socket() failed: %s", strerror(errno));
        return -1;
    }

    int reuse = 1;
    setsockopt(g_listen_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    if (bind(g_listen_fd, reinterpret_cast<struct sockaddr *>(&addr), sizeof(addr)) < 0)
    {
        LOG_ERROR(TAG, "bind() failed on port %u: %s", port, strerror(errno));
        close(g_listen_fd);
        g_listen_fd = -1;
        return -1;
    }

    if (listen(g_listen_fd, RTSP_MAX_SESSIONS) < 0)
    {
        LOG_ERROR(TAG, "listen() failed: %s", strerror(errno));
        close(g_listen_fd);
        g_listen_fd = -1;
        return -1;
    }

    rtsp_session_registry_init();

    g_running = true;

    if (pthread_create(&g_accept_thread, nullptr, accept_thread_func, nullptr) != 0)
    {
        LOG_ERROR(TAG, "pthread_create (accept) failed: %s", strerror(errno));
        close(g_listen_fd);
        g_listen_fd = -1;
        g_running = false;
        return -1;
    }

    if (pthread_create(&g_reaper_thread, nullptr, reaper_thread_func, nullptr) != 0)
    {
        LOG_ERROR(TAG, "pthread_create (reaper) failed: %s", strerror(errno));
        g_running = false;
        pthread_join(g_accept_thread, nullptr);
        close(g_listen_fd);
        g_listen_fd = -1;
        return -1;
    }

    LOG_INFO(TAG, "listening on port %u (max %d sessions)", port, RTSP_MAX_SESSIONS);

    return 0;
}

void rtsp_server_stop(void)
{
    g_running = false;

    if (g_listen_fd >= 0)
    {
        shutdown(g_listen_fd, SHUT_RDWR);
        close(g_listen_fd);
        g_listen_fd = -1;
    }

    pthread_join(g_accept_thread, nullptr);
    pthread_join(g_reaper_thread, nullptr);

    rtsp_session_registry_cleanup();

    g_rtsp_username.clear();
    g_rtsp_password.clear();
}
