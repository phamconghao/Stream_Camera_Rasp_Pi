#include "rtsp_server.h"

#include <pthread.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cerrno>
#include <atomic>
#include <sstream>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <unistd.h>

#include "rtsp_message.h"
#include "rtsp_session_registry.h"
#include "pipeline_controller.h"
#include "udp_sender.h"
#include "sps_pps_cache.h"
#include "base64.h"
#include "log.h"

static const char *TAG = "RTSP_SRV";

static int g_listen_fd = -1;
static pthread_t g_accept_thread;
static pthread_t g_reaper_thread;
static std::atomic<bool> g_running(false);

static constexpr int REAPER_INTERVAL_SEC = 10;

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

static rtsp_response_t dispatch(const rtsp_request_t &req, const std::string &client_ip)
{
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

            rtsp_response_t resp = dispatch(req, client_ip);
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

int rtsp_server_start(uint16_t port)
{
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
}
