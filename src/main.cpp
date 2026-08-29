#include <iostream>
#include <cstdlib>
#include <cstdint>
#include <string>
#include <unistd.h>

#include "app_state.h"
#include "control_listener_thread.h"
#include "rtcp_sender_thread.h"
#include "rtsp_server.h"
#include "pipeline_controller.h"
#include "signaling_server.h"
#include "webrtc_sdp.h"
#include "webrtc_media_registry.h"
#include "dtls_cert.h"
#include "ice_credentials.h"
#include "ice_candidate.h"
#include "ice_agent.h"
#include "turn_client.h"
#include "dtls_handshake.h"
#include "srtp_session.h"
#include "sps_pps_cache.h"
#include "json_lite.h"
#include "log.h"

#include <map>
#include <mutex>
#include <vector>
#include <sstream>

/**
 * ============================================================================
 * FULL PIPELINE MAP (see individual files for per-stage detail comments)
 * ============================================================================
 *
 *   Camera (libcamera)
 *     |
 *   Capture         <- camera_capture.{h,cpp}            (no dedicated thread; driven by libcamera's own callback)
 *     |
 *   Raw Frame Pool/Queue   <- frame/raw_frame*.{h,cpp}
 *     |
 *   Encoder Thread  <- encoder/encoder_thread.{h,cpp} + encoder/bcm2835_encoder.{h,cpp}
 *     |
 *   Encoded Frame Pool/Queue  <- frame/encoded_frame*.{h,cpp}
 *     |
 *   RTP Packetizer Thread  <- rtp/rtp_packetizer_thread.{h,cpp}
 *     |    (uses parser/h264_nal_parser.{h,cpp} + rtp/rtp_packetizer.{h,cpp})
 *     |
 *   RTP Packet Pool/Queue  <- rtp/rtp_packet*.{h,cpp}
 *     |
 *   UDP Sender Thread  <- network/udp_sender_thread.{h,cpp} + network/udp_sender.{h,cpp}
 *     |
 *   Network -> RTSP/RTP Client
 *
 * Every arrow above is "producer pushes pointer, consumer pops pointer" -
 * frame/packet bytes are never copied between pipeline stages (only
 * copied once, when captured from the camera / read out of the
 * hardware encoder).
 *
 * The data pipeline above is not started unconditionally at process
 * startup. rtsp/pipeline_controller.{h,cpp} owns its lifecycle
 * entirely - main() just prepares it (hardware init, pool/queue alloc)
 * via pipeline_controller_init(), then the RTSP server's PLAY/TEARDOWN
 * handlers (rtsp_server.cpp) call pipeline_controller_ensure_running()/
 * _release() as clients come and go. The control channel, RTCP, and the
 * RTSP control plane itself are not lazy - they listen continuously
 * from process startup, independent of whether the data pipeline is
 * currently running, so a client can always connect/DESCRIBE/SETUP even
 * before anyone has PLAYed.
 *
 * CONTROL CHANNEL (packet-loss recovery + adaptive bitrate), separate
 * from the RTP data path above: network/control_listener_thread.{h,cpp}
 * listens on `control_port` for two kinds of UDP messages from the
 * receiver's control_channel (network/control_channel.h):
 *   - keyframe request (sent whenever the receiver's jitter buffer
 *     detects lost packets) -> bcm2835_encoder_force_keyframe(), so the
 *     receiver recovers a clean picture quickly instead of waiting for
 *     the next regularly-scheduled IDR.
 *   - loss report (sent periodically by the receiver's
 *     loss_reporter_thread) -> maps the reported loss rate to a target
 *     bitrate tier and applies it via bcm2835_encoder_set_bitrate() if
 *     it changed, so the stream backs off automatically under bad
 *     network conditions instead of continuing to send at a rate the
 *     link can't sustain.
 *
 * RTCP: network/rtcp_sender_thread.{h,cpp} periodically
 * injects a Sender Report (RFC 3550 6.4.1) into the SAME socket
 * udp_sender_thread uses for RTP data (rtcp-mux, RFC 5761 - no new
 * port). This is genuine RFC 3550 wire format (interoperable with
 * tools like Wireshark), separate from and in addition to the ad-hoc
 * control_channel messages above.
 *
 * RTP FAN-OUT: udp_sender sends every packet to the destination of
 * each currently-PLAYING RTSP session (rtsp_server.cpp's
 * handle_play()/handle_teardown() register/deregister them by
 * session_id) - see udp_sender.h and pipeline_controller.h.
 *
 * WEBRTC SIGNALING (separate from everything above): a second,
 * independent TCP server - signaling/signaling_server.{h,cpp} -
 * listens on `signaling_port` for WebSocket connections from a
 * browser's RTCPeerConnection, used to exchange SDP offer/answer and
 * ICE candidates. See webrtc_sdp.h, ice_agent.h, dtls_handshake.h, and
 * srtp_session.h for the rest of the WebRTC media path.
 */

// How long to wait for a freshly-primed pipeline to produce SPS/PPS
// before giving up on an offer - same technique and timeout as
// handle_describe()'s priming (see rtsp_server.cpp), duplicated here
// since this is a different TCP server (signaling, not RTSP) with no
// code path connecting the two.
static constexpr int OFFER_PRIME_MAX_WAIT_ITERATIONS = 20;
static constexpr int OFFER_PRIME_WAIT_STEP_MS = 50;

// Fixed UDP port ice_agent listens on for STUN Binding Requests -
// shared across every concurrent peer connection (sessions are told
// apart by ice_ufrag, not by port - see ice_agent.h).
static constexpr uint16_t ICE_AGENT_PORT = 40000;

// Which ice_ufrag a given signaling client_id's WebRTC session is -
// the only link this project has between "a WebSocket connection
// closed" (signaling_server.h's disconnect handler, keyed by
// client_id) and "which ICE/DTLS/SRTP/media session to tear down"
// (everything else in this file is keyed by ice_ufrag instead, since
// that's what's shared with the browser over signaling). A simple
// mutex-guarded map, same pattern as every other small registry in
// this project.
static std::mutex g_client_ufrag_lock;
static std::map<std::string, std::string> g_client_to_ufrag;

// Called when a signaling WebSocket disconnects - the only WebRTC
// session-end signal this project currently detects (see
// signaling_server.h's disconnect handler doc comment for the known
// limitation: an ICE-level failure/timeout with the WebSocket still
// open isn't caught by this). Tears down everything that session's
// resources touched - ICE, DTLS/SRTP (which itself releases the
// pipeline ref-count taken in dtls_handshake.cpp, if any was taken),
// and the client_id<->ufrag mapping itself.
static void on_signaling_disconnect(const std::string &client_id)
{
    std::string ufrag;

    {
        std::lock_guard<std::mutex> lock(g_client_ufrag_lock);
        auto it = g_client_to_ufrag.find(client_id);
        if (it == g_client_to_ufrag.end())
        {
            return; // this client never completed an offer (e.g. connected then disconnected immediately) - nothing to tear down
        }
        ufrag = it->second;
        g_client_to_ufrag.erase(it);
    }

    LOG_INFO("MAIN", "client %s disconnected - tearing down WebRTC session ufrag=%s", client_id.c_str(), ufrag.c_str());

    ice_agent_unregister_session(ufrag);
    dtls_handshake_unregister_session(ufrag); // also releases the pipeline ref-count if this session had reached "media ready" (see dtls_handshake.cpp)
}

// Dispatches a parsed "offer" message to the SDP answer builder
// (webrtc_sdp.h) and sends the "answer" back over the same signaling
// connection. Nothing data-related (RTP/SRTP) happens as a result of
// this - the answer just tells the browser how to reach this project;
// ICE/DTLS/SRTP run afterward, driven by the browser's own connection
// attempt.
static void handle_offer(const std::string &client_id, const std::string &sdp)
{
    webrtc_sdp_offer_t offer = parse_webrtc_sdp_offer(sdp);
    if (!offer.valid)
    {
        LOG_WARN("MAIN", "malformed/incomplete offer from client %s (missing ICE/fingerprint/mid, or no H.264 packetization-mode=1 entry found)", client_id.c_str());
        signaling_server_send(client_id, json_build_object({{"type", "error"}, {"message", "invalid or incomplete SDP offer"}}));
        return;
    }

    // Same "prime the pipeline just long enough to get one SPS/PPS"
    // trick as handle_describe() uses - a browser can just as easily
    // be the very first connection of the process as an RTSP client
    // can.
    bool primed_pipeline = false;
    if (!sps_pps_cache_has_both())
    {
        pipeline_controller_ensure_running();
        primed_pipeline = true;

        for (int i = 0; i < OFFER_PRIME_MAX_WAIT_ITERATIONS && !sps_pps_cache_has_both(); i++)
        {
            usleep(OFFER_PRIME_WAIT_STEP_MS * 1000);
        }
    }

    if (!sps_pps_cache_has_both())
    {
        if (primed_pipeline)
        {
            pipeline_controller_release();
        }

        LOG_WARN("MAIN", "no SPS/PPS available after priming wait, rejecting offer from %s", client_id.c_str());
        signaling_server_send(client_id, json_build_object({{"type", "error"}, {"message", "encoder not ready"}}));
        return;
    }

    std::vector<uint8_t> sps = sps_pps_cache_get_sps();
    std::vector<uint8_t> pps = sps_pps_cache_get_pps();

    if (primed_pipeline)
    {
        pipeline_controller_release();
    }

    ice_credentials_t ice = generate_ice_credentials();
    std::string fingerprint = dtls_cert_get_fingerprint_sha256();

    std::string answer_sdp = build_webrtc_sdp_answer(ice.ufrag, ice.pwd, fingerprint, offer.mid, offer.h264_payload_type, sps, pps);

    // Record this session's negotiated payload type now, at offer
    // time - well before the session reaches "media ready" (DTLS/SRTP
    // connect), so webrtc_sender_thread.cpp always has it available by
    // the time there's ever anything to send this session.
    webrtc_media_registry_set_payload_type(ice.ufrag, offer.h264_payload_type);

    // This session's STUN requests won't validate against anything
    // until its (ufrag, pwd) is registered with ice_agent - must
    // happen before the browser could possibly start sending checks,
    // so it's done here, before the answer (which is what triggers
    // the browser to start ICE) is even sent.
    ice_agent_register_session(ice.ufrag, ice.pwd);

    // This session's DTLS handshake (whenever the browser actually
    // starts it, over the ICE-verified UDP path above) needs to know
    // which certificate fingerprint to expect from the browser -
    // straight from the offer this project just parsed.
    dtls_handshake_register_session(ice.ufrag, offer.fingerprint_algo, offer.fingerprint_hex);

    // Remember which ufrag belongs to this signaling client_id, so
    // on_signaling_disconnect() can find it later when this client's
    // WebSocket closes.
    {
        std::lock_guard<std::mutex> lock(g_client_ufrag_lock);
        g_client_to_ufrag[client_id] = ice.ufrag;
    }

    LOG_INFO("MAIN", "sending answer to client %s (ice-ufrag=%s)", client_id.c_str(), ice.ufrag.c_str());

    signaling_server_send(client_id, json_build_object({{"type", "answer"}, {"sdp", answer_sdp}}));

    // Tell the browser where to send its connectivity checks. Sent
    // as its own message right after the answer, rather than embedded
    // inside the answer's SDP, so this project's signaling protocol
    // matches how trickle ICE actually works in practice (candidates
    // as a separate, potentially repeated, message type - see
    // signaling_server.h). Phase 24.2: this project can now have more
    // than one local interface worth advertising (e.g. LAN + a
    // Tailscale VPN interface), so every candidate from
    // get_local_host_candidates() is sent as its own "ice-candidate"
    // message - the browser's ICE agent tries all of them and uses
    // whichever pair actually connects.
    std::vector<ice_candidate_t> local_candidates = get_local_host_candidates(ICE_AGENT_PORT);

    // Phase 24.3: append a server-reflexive candidate too, if the
    // one-time STUN discovery in ice_agent_start() succeeded - this is
    // what lets a browser with no VPN and no LAN access reach the Pi
    // at all. Absent (rather than fatal) when discovery failed/hasn't
    // completed - LAN/Tailscale viewers are unaffected either way.
    ice_public_address_t public_address;
    if (ice_agent_get_public_address(public_address))
    {
        local_candidates.push_back(make_server_reflexive_candidate(
            public_address.public_ip, public_address.public_port,
            public_address.base_ip, public_address.base_port));
    }

    // Phase 24.4: append a relay candidate too, if turn_client_allocate()
    // succeeded at startup - the last-resort path for NATs neither a
    // host nor a server-reflexive candidate can get through.
    turn_relay_address_t relay_address;
    if (turn_client_get_relay_address(relay_address))
    {
        // TURN's own server address, not this project's - see
        // ice_candidate.h's make_relay_candidate() doc comment on what
        // raddr/rport mean for a relay candidate specifically. Only
        // known here as "whatever host:port env vars pointed at",
        // hence the getenv() re-read rather than threading it through
        // turn_client's own state.
        const char *turn_host = getenv("TURN_SERVER_HOST");
        const char *turn_port_str = getenv("TURN_SERVER_PORT");
        local_candidates.push_back(make_relay_candidate(
            relay_address.ip, relay_address.port,
            turn_host != nullptr ? turn_host : "",
            turn_port_str != nullptr ? static_cast<uint16_t>(atoi(turn_port_str)) : 0));
    }

    if (!local_candidates.empty())
    {
        for (const ice_candidate_t &local_candidate : local_candidates)
        {
            std::string candidate_line = build_ice_candidate_line(local_candidate);
            const char *label = local_candidate.kind == ice_candidate_kind_t::SERVER_REFLEXIVE
                                     ? " (public/srflx)"
                                     : (local_candidate.kind == ice_candidate_kind_t::RELAY
                                            ? " (turn/relay)"
                                            : (local_candidate.is_tailscale ? " (tailscale)" : " (lan)"));
            signaling_server_send(client_id, json_build_object({
                {"type", "ice-candidate"},
                {"candidate", candidate_line},
                {"sdpMid", offer.mid},
                {"sdpMLineIndex", "0"},
            }));
            LOG_INFO("MAIN", "sent ICE candidate to client %s: %s%s", client_id.c_str(), candidate_line.c_str(), label);
        }
    }
    else
    {
        // Not fatal to the offer/answer exchange itself - but without
        // a candidate the browser has nowhere to send connectivity
        // checks, so ICE (and therefore the whole connection) simply
        // won't progress past this point. Most likely cause: no
        // non-loopback IPv4 interface found (see ice_candidate.cpp).
        LOG_WARN("MAIN", "no local host candidate available - browser %s has nowhere to send STUN checks", client_id.c_str());
    }
}

// PHASE 24.4: pulls the connection-address field out of an SDP/trickle
// -ICE "candidate:" line (RFC 8839 section 5.1 - "candidate" SP
// foundation SP component-id SP transport SP priority SP
// connection-address SP port SP "typ" ...), i.e. the 5th
// space-separated token after the "candidate:" prefix. Returns "" if
// the line doesn't look like a well-formed candidate line at all -
// callers treat that as "nothing to do", not an error.
static std::string extract_candidate_ip(const std::string &candidate_line)
{
    std::istringstream stream(candidate_line);
    std::string token;
    int token_index = 0;

    while (stream >> token)
    {
        if (token_index == 4) // 0=foundation(with "candidate:" prefix), 1=component, 2=transport, 3=priority, 4=address
        {
            return token;
        }
        token_index++;
    }

    return "";
}

// Dispatches incoming signaling messages by type. "offer" triggers the
// full answer/candidate exchange (handle_offer()); "ice-candidate"
// (the browser's own candidates) feeds turn_client_create_permission()
// so relayed traffic from the browser has somewhere to go once it
// starts arriving - see turn_client.h's doc comment on why this has to
// happen BEFORE that traffic shows up, not reactively.
static void on_signaling_message(const std::string &client_id, const std::string &raw_json)
{
    std::map<std::string, std::string> fields = json_parse_object(raw_json);

    auto it = fields.find("type");
    std::string type = (it != fields.end()) ? it->second : "";

    if (type == "offer")
    {
        auto sdp_it = fields.find("sdp");
        if (sdp_it == fields.end())
        {
            LOG_WARN("MAIN", "offer from %s missing \"sdp\" field", client_id.c_str());
            signaling_server_send(client_id, json_build_object({{"type", "error"}, {"message", "offer missing sdp field"}}));
            return;
        }
        handle_offer(client_id, sdp_it->second);
    }
    else if (type == "ice-candidate")
    {
        auto candidate_it = fields.find("candidate");
        std::string peer_ip = (candidate_it != fields.end()) ? extract_candidate_ip(candidate_it->second) : "";

        if (peer_ip.empty())
        {
            LOG_WARN("MAIN", "ice-candidate from %s missing/unparseable \"candidate\" field, ignoring", client_id.c_str());
            return;
        }

        turn_relay_address_t relay_address;
        if (turn_client_get_relay_address(relay_address))
        {
            // Best-effort - failure here just means the relay path
            // specifically won't work for THIS candidate's address
            // (host/srflx candidates the browser also sent are
            // unaffected, and most sessions never end up needing the
            // relay path at all - see ice_candidate.cpp's priority
            // ordering, relay is only ever picked as a last resort).
            if (!turn_client_create_permission(peer_ip))
            {
                LOG_WARN("MAIN", "CreatePermission failed for browser candidate %s (client %s) - "
                                  "relay path won't work for this peer if it comes to that",
                         peer_ip.c_str(), client_id.c_str());
            }
        }
        // else: no TURN allocation this run - nothing to do, the
        // browser's own candidates are otherwise unused by this
        // project (see the original comment this replaced, still true
        // for the direct/reflexive path: this side derives the
        // checked address from each incoming STUN request instead of
        // needing the browser's candidates in advance for that part).
    }
    else
    {
        // Anything else this project doesn't have a handler for -
        // logged so the exchange itself can still be observed/debugged.
        LOG_INFO("MAIN", "signaling message from client %s: type=%s (not yet handled)", client_id.c_str(), type.c_str());
    }
}

int main(int argc, char **argv)
{
    // Control channel port this sender listens on for keyframe-request
    // datagrams from the receiver (see control_listener_thread.h).
    uint16_t control_port = (argc > 1) ? static_cast<uint16_t>(std::atoi(argv[1])) : 5005;

    // TCP port the RTSP control plane listens on.
    uint16_t rtsp_port = (argc > 2) ? static_cast<uint16_t>(std::atoi(argv[2])) : 8554;

    // TCP port the WebRTC signaling (WebSocket) server listens on
    // independent of rtsp_port above.
    uint16_t signaling_port = (argc > 3) ? static_cast<uint16_t>(std::atoi(argv[3])) : 8765;

    // PHASE 23.2: control-channel HMAC secret - deliberately an env
    // var, not a positional argv (argv is visible to any local user via
    // /proc/<pid>/cmdline or `ps`; an env var set in a systemd unit
    // file or shell profile isn't). Required, not optional with a
    // built-in default - a default baked into the binary would be
    // public the moment this project's source is, defeating the point.
    // Must match what camera_receiver's CAMERA_CONTROL_SECRET is set
    // to, or every control message it sends will be rejected.
    const char *control_secret_env = std::getenv("CAMERA_CONTROL_SECRET");
    if (control_secret_env == nullptr || control_secret_env[0] == '\0')
    {
        std::cerr << "CAMERA_CONTROL_SECRET environment variable must be set "
                     "(shared secret for control-channel HMAC authentication, "
                     "see docs-security-threat-model.md) - refusing to start "
                     "with no authentication on the control channel.\n";
        return -1;
    }
    std::string control_secret(control_secret_env);

    // PHASE 23.3: RTSP Digest Authentication credentials - same
    // fail-closed reasoning as CAMERA_CONTROL_SECRET above (env var,
    // not argv; required, no built-in default). Checked here,
    // alongside the control-channel secret, so the process fails fast
    // before allocating any pipeline/thread resources rather than
    // partway through startup.
    const char *rtsp_username_env = std::getenv("RTSP_USERNAME");
    const char *rtsp_password_env = std::getenv("RTSP_PASSWORD");
    if (rtsp_username_env == nullptr || rtsp_username_env[0] == '\0' ||
        rtsp_password_env == nullptr || rtsp_password_env[0] == '\0')
    {
        std::cerr << "RTSP_USERNAME and RTSP_PASSWORD environment variables must both be set "
                     "(credentials for RTSP Digest Authentication, see "
                     "docs-security-threat-model.md) - refusing to start with no "
                     "authentication on the RTSP server.\n";
        return -1;
    }
    std::string rtsp_username(rtsp_username_env);
    std::string rtsp_password(rtsp_password_env);

    // PHASE 23.4: WebSocket signaling pre-shared token - same
    // fail-closed reasoning as the two checks above. Checked here too,
    // before any resource allocation, for the same fail-fast reason.
    const char *signaling_token_env = std::getenv("SIGNALING_TOKEN");
    if (signaling_token_env == nullptr || signaling_token_env[0] == '\0')
    {
        std::cerr << "SIGNALING_TOKEN environment variable must be set "
                     "(pre-shared token for WebSocket signaling authentication, "
                     "see docs-security-threat-model.md) - refusing to start "
                     "with no authentication on the signaling server.\n";
        return -1;
    }
    std::string signaling_token(signaling_token_env);

    // App-level flag: only main() (or a future signal handler installed
    // by main()) writes to this. Each thread module manages its own
    // independent running flag for start/stop, so they can be controlled
    // separately later (per RTSP-client sessions, etc.).
    g_running = true;

    // Hardware init + pool/queue allocation only - does NOT start
    // streaming yet. The data pipeline itself is started lazily, on
    // the first RTSP PLAY - see pipeline_controller.h. RTP destinations
    // are no longer fixed here either (see udp_sender.h) - each PLAYING
    // RTSP session registers its own.
    if (pipeline_controller_init() < 0)
    {
        return -1;
    }

    // These three listen continuously from startup, independent of
    // whether any client has PLAYed yet.
    if (control_listener_thread_start(control_port, control_secret) < 0)
    {
        return -1;
    }

    if (rtcp_sender_thread_start() < 0)
    {
        return -1;
    }

    if (rtsp_server_start(rtsp_port, rtsp_username, rtsp_password) < 0)
    {
        return -1;
    }

    // One self-signed DTLS cert for the whole process lifetime,
    // generated before the signaling server starts accepting offers
    // (its fingerprint is required to build every SDP answer - see
    // webrtc_sdp.h).
    if (dtls_cert_init() < 0)
    {
        return -1;
    }

    // Builds the shared SSL_CTX (cert/key from dtls_cert_init()
    // above, SRTP profile negotiation enabled) every DTLS session's
    // handshake uses - must happen after dtls_cert_init() and before
    // any offer could arrive (same "ready before it's needed"
    // reasoning as ice_agent_start() below).
    if (dtls_handshake_init() < 0)
    {
        return -1;
    }

    // libsrtp2's own one-time global init - must happen before
    // dtls_handshake.cpp's first DTLS handshake could possibly
    // complete and try to call srtp_session_create().
    if (srtp_session_manager_init() < 0)
    {
        return -1;
    }

    signaling_server_set_disconnect_handler(on_signaling_disconnect);

    if (signaling_server_start(signaling_port, signaling_token, on_signaling_message) < 0)
    {
        return -1;
    }

    // Must be listening before any offer could possibly be answered
    // (handle_offer() registers each session's credentials with this,
    // and sends the browser a candidate pointing at ICE_AGENT_PORT -
    // both are meaningless if nothing is bound there yet).
    if (ice_agent_start(ICE_AGENT_PORT) < 0)
    {
        return -1;
    }

    // PHASE 24.4: TURN relay - entirely optional, and off unless every
    // one of these env vars is set (unlike CAMERA_CONTROL_SECRET's
    // fail-closed requirement in control_listener_thread.cpp, there's
    // nothing insecure about running without a TURN server - it just
    // means no relay candidate gets offered, same as if 24.3's STUN
    // discovery fails). A failed Allocate is logged by turn_client.cpp
    // itself and is likewise non-fatal to startup.
    const char *turn_host = getenv("TURN_SERVER_HOST");
    const char *turn_port_str = getenv("TURN_SERVER_PORT");
    const char *turn_username = getenv("TURN_USERNAME");
    const char *turn_password = getenv("TURN_PASSWORD");

    bool turn_enabled = false;
    if (turn_host != nullptr && turn_port_str != nullptr && turn_username != nullptr && turn_password != nullptr)
    {
        turn_relay_address_t relay;
        uint16_t turn_port = static_cast<uint16_t>(atoi(turn_port_str));

        if (turn_client_allocate(turn_host, turn_port, turn_username, turn_password, relay))
        {
            // Wire relayed STUN checks into the same handling the
            // direct/reflexive path uses - see ice_agent.h's Phase
            // 24.4 doc comment on ice_agent_handle_relayed_packet().
            turn_client_set_data_callback(ice_agent_handle_relayed_packet);
            turn_enabled = true;
            LOG_INFO("MAIN", "TURN relay candidate available: %s:%u", relay.ip.c_str(), relay.port);
        }
    }
    else
    {
        LOG_INFO("MAIN", "TURN_SERVER_HOST/PORT/USERNAME/PASSWORD not fully set - no relay candidate this run");
    }

    std::cout << "RTSP server ready at rtsp://<this-pi-ip>:" << rtsp_port << "/stream" << std::endl;
    std::cout << "WebRTC signaling server ready at ws://<this-pi-ip>:" << signaling_port << std::endl;
    std::cout << "Press ENTER to exit..." << std::endl;

    std::cin.get();

    g_running = false;

    // Stop the always-on services first, then let pipeline_controller
    // force-stop the data pipeline if any RTSP client was still PLAYing
    // (its cleanup() is defensive about this - see pipeline_controller.cpp).
    control_listener_thread_stop();

    rtcp_sender_thread_stop();

    rtsp_server_stop();

    signaling_server_stop();

    ice_agent_stop();

    if (turn_enabled)
    {
        turn_client_deallocate();
    }

    dtls_handshake_cleanup();

    srtp_session_manager_cleanup();

    dtls_cert_cleanup();

    pipeline_controller_cleanup();

    return 0;
}
