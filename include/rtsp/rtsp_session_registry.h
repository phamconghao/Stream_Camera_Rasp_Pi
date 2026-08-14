#ifndef __RTSP_SESSION_REGISTRY_H__
#define __RTSP_SESSION_REGISTRY_H__

#include <string>
#include <vector>
#include <cstdint>

/**
 * ============================================================================
 * PHASE 20: Per-client session bookkeeping - replaces the fixed
 * dest_ip/dest_port CLI arguments the sender currently uses.
 * ============================================================================
 *
 * Thread-safe: SETUP/PLAY/TEARDOWN each arrive on their own per-connection
 * thread (see rtsp_server.cpp), so every function here takes an internal
 * lock. Session count is hard-capped at RTSP_MAX_SESSIONS (5) - SETUP
 * requests beyond that are rejected with 453 Not Enough Bandwidth by
 * the caller (rtsp_server.cpp), not by this module.
 */

enum class rtsp_session_state_t
{
    INIT,
    READY,
    PLAYING,
};

struct rtsp_session_t
{
    std::string session_id;
    rtsp_session_state_t state;
    std::string client_ip;
    uint16_t client_rtp_port;
    uint16_t client_rtcp_port;
    uint64_t last_activity_us;
};

static constexpr int RTSP_MAX_SESSIONS = 5;
static constexpr uint64_t RTSP_SESSION_TIMEOUT_US = 60ULL * 1000000ULL; // 60s

void rtsp_session_registry_init(void);
void rtsp_session_registry_cleanup(void);

std::string rtsp_session_registry_create(const std::string &client_ip,
                                          uint16_t client_rtp_port,
                                          uint16_t client_rtcp_port);

bool rtsp_session_registry_set_state(const std::string &session_id, rtsp_session_state_t state);
bool rtsp_session_registry_get_state(const std::string &session_id, rtsp_session_state_t *out_state);

// Phase 20 step 4 (RTP fan-out): full session lookup, used by
// rtsp_server.cpp's handle_play() to fetch the client_ip/client_rtp_port
// a PLAYING session's RTP destination should be added to udp_sender under.
bool rtsp_session_registry_get(const std::string &session_id, rtsp_session_t *out_session);

bool rtsp_session_registry_touch(const std::string &session_id);
bool rtsp_session_registry_remove(const std::string &session_id);
bool rtsp_session_registry_exists(const std::string &session_id);

int rtsp_session_registry_count(void);

/**
 * Removes every session whose last_activity is older than
 * RTSP_SESSION_TIMEOUT_US. Returns the removed sessions themselves (not
 * just a count) so the caller (rtsp_server.cpp's reaper thread) can
 * call pipeline_controller_release() for any that were PLAYING - a
 * client that crashes mid-stream without sending TEARDOWN would
 * otherwise leave the pipeline running forever with a phantom viewer.
 */
std::vector<rtsp_session_t> rtsp_session_registry_reap_orphans(void);

#endif // __RTSP_SESSION_REGISTRY_H__
