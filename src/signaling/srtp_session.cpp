#include "srtp_session.h"

#include <pthread.h>
#include <map>
#include <cstring>

#include <srtp2/srtp.h>

#include "log.h"

static const char *TAG = "SRTP_SESSION";

// RFC 5764 section 4.2's fixed byte layout - see srtp_session.h's
// srtp_derived_keys_t doc comment.
static constexpr size_t SRTP_DERIVE_KEY_LEN = 16;
static constexpr size_t SRTP_DERIVE_SALT_LEN = 14;
static constexpr size_t SRTP_KEYING_MATERIAL_LEN = SRTP_DERIVE_KEY_LEN * 2 + SRTP_DERIVE_SALT_LEN * 2; // 60

srtp_derived_keys_t derive_srtp_keys(const std::vector<uint8_t> &keying_material)
{
    srtp_derived_keys_t keys;

    if (keying_material.size() != SRTP_KEYING_MATERIAL_LEN)
    {
        LOG_ERROR(TAG, "keying material is %zu bytes, expected exactly %zu - cannot derive SRTP keys",
                   keying_material.size(), SRTP_KEYING_MATERIAL_LEN);
        return keys; // every vector stays empty - see header comment
    }

    size_t pos = 0;

    keys.client_write_key.assign(keying_material.begin() + pos, keying_material.begin() + pos + SRTP_DERIVE_KEY_LEN);
    pos += SRTP_DERIVE_KEY_LEN;

    keys.server_write_key.assign(keying_material.begin() + pos, keying_material.begin() + pos + SRTP_DERIVE_KEY_LEN);
    pos += SRTP_DERIVE_KEY_LEN;

    keys.client_write_salt.assign(keying_material.begin() + pos, keying_material.begin() + pos + SRTP_DERIVE_SALT_LEN);
    pos += SRTP_DERIVE_SALT_LEN;

    keys.server_write_salt.assign(keying_material.begin() + pos, keying_material.begin() + pos + SRTP_DERIVE_SALT_LEN);
    pos += SRTP_DERIVE_SALT_LEN;

    return keys;
}

struct srtp_session_t
{
    srtp_t send_ctx = nullptr; // encrypts OUR outbound traffic, keyed with server_write_key/salt (this project is always the DTLS server - a=setup:passive)
    srtp_t recv_ctx = nullptr; // decrypts the BROWSER's inbound traffic, keyed with client_write_key/salt

    // TEMP DIAGNOSTIC (remove after root cause found): kept around so
    // srtp_session_debug_verify_roundtrip() can build a one-off verify
    // context using this session's REAL server_write key/salt, to
    // round-trip actual outbound packets (not just a synthetic one).
    std::vector<uint8_t> debug_server_write_key;
    std::vector<uint8_t> debug_server_write_salt;
};

static pthread_mutex_t g_sessions_lock = PTHREAD_MUTEX_INITIALIZER;
static std::map<std::string, srtp_session_t *> g_sessions; // ice_ufrag -> session
static bool g_manager_initialized = false;

int srtp_session_manager_init(void)
{
    if (srtp_init() != srtp_err_status_ok)
    {
        LOG_ERROR(TAG, "srtp_init() failed");
        return -1;
    }

    g_manager_initialized = true;
    LOG_INFO(TAG, "libsrtp2 initialized");

    return 0;
}

void srtp_session_manager_cleanup(void)
{
    pthread_mutex_lock(&g_sessions_lock);

    for (auto &pair : g_sessions)
    {
        if (pair.second->send_ctx)
        {
            srtp_dealloc(pair.second->send_ctx);
        }
        if (pair.second->recv_ctx)
        {
            srtp_dealloc(pair.second->recv_ctx);
        }
        delete pair.second;
    }
    g_sessions.clear();

    pthread_mutex_unlock(&g_sessions_lock);

    if (g_manager_initialized)
    {
        srtp_shutdown();
        g_manager_initialized = false;
    }
}

// Builds one direction's srtp_t context - `key` must be exactly
// SRTP_DERIVE_KEY_LEN bytes and `salt` exactly SRTP_DERIVE_SALT_LEN bytes (both
// guaranteed by derive_srtp_keys() above). `is_outbound` selects
// ssrc_any_outbound (this project protecting what IT sends) vs
// ssrc_any_inbound (this project unprotecting what IT receives) -
// "any" rather than a specific SSRC since this project doesn't
// pre-negotiate SSRC values with the browser at this phase.
static srtp_t create_srtp_ctx(const std::vector<uint8_t> &key, const std::vector<uint8_t> &salt, bool is_outbound)
{
    // libsrtp2's master key buffer is key||salt concatenated
    // (SRTP_MASTER_KEY_LEN = 30 = 16 + 14) - see srtp.h's
    // SRTP_AES_ICM_128_KEY_LEN_WSALT.
    uint8_t master_key[SRTP_MASTER_KEY_LEN];
    memcpy(master_key, key.data(), SRTP_DERIVE_KEY_LEN);
    memcpy(master_key + SRTP_DERIVE_KEY_LEN, salt.data(), SRTP_DERIVE_SALT_LEN);

    srtp_policy_t policy;
    memset(&policy, 0, sizeof(policy));

    // SRTP_AES128_CM_SHA1_80 (dtls_handshake.cpp's negotiated
    // profile) is libsrtp2's DEFAULT crypto policy - no separate
    // profile selection needed here, these two calls already produce
    // exactly that.
    srtp_crypto_policy_set_rtp_default(&policy.rtp);
    srtp_crypto_policy_set_rtcp_default(&policy.rtcp);

    policy.ssrc.type = is_outbound ? ssrc_any_outbound : ssrc_any_inbound;
    policy.key = master_key;
    policy.next = nullptr;

    srtp_t ctx = nullptr;
    srtp_err_status_t status = srtp_create(&ctx, &policy);

    // master_key held sensitive bytes - wipe rather than just letting
    // it fall out of scope, even though this is a small/short-lived
    // stack buffer.
    memset(master_key, 0, sizeof(master_key));

    if (status != srtp_err_status_ok)
    {
        LOG_ERROR(TAG, "srtp_create() failed (status=%d, outbound=%d)", static_cast<int>(status), is_outbound);
        return nullptr;
    }

    return ctx;
}

int srtp_session_create(const std::string &ice_ufrag, const std::vector<uint8_t> &keying_material)
{
    srtp_derived_keys_t keys = derive_srtp_keys(keying_material);
    if (keys.server_write_key.empty())
    {
        return -1; // derive_srtp_keys() already logged why
    }

    srtp_session_t *session = new srtp_session_t();

    session->send_ctx = create_srtp_ctx(keys.server_write_key, keys.server_write_salt, true);
    session->recv_ctx = create_srtp_ctx(keys.client_write_key, keys.client_write_salt, false);
    session->debug_server_write_key = keys.server_write_key;
    session->debug_server_write_salt = keys.server_write_salt;

    if (!session->send_ctx || !session->recv_ctx)
    {
        if (session->send_ctx) srtp_dealloc(session->send_ctx);
        if (session->recv_ctx) srtp_dealloc(session->recv_ctx);
        delete session;
        return -1;
    }

    pthread_mutex_lock(&g_sessions_lock);

    // Replace, don't leak, if a session for this ufrag already
    // existed (e.g. a re-negotiation) - shouldn't normally happen
    // within this phase's scope, but safe regardless.
    auto it = g_sessions.find(ice_ufrag);
    if (it != g_sessions.end())
    {
        if (it->second->send_ctx) srtp_dealloc(it->second->send_ctx);
        if (it->second->recv_ctx) srtp_dealloc(it->second->recv_ctx);
        delete it->second;
    }

    g_sessions[ice_ufrag] = session;
    pthread_mutex_unlock(&g_sessions_lock);

    LOG_INFO(TAG, "SRTP session created for ufrag=%s (send + recv contexts ready)", ice_ufrag.c_str());

    // TEMP DIAGNOSTIC (remove after root cause found): round-trip a
    // fake RTP packet through send_ctx (protect) and a fresh context
    // built with the SAME server_write key/salt (unprotect) - exactly
    // what the browser does with everything this project actually
    // sends it. If this fails, the bug is in this project's own SRTP
    // usage; if it passes, the bug is downstream of encryption
    // entirely (network, or the browser's own H.264 pipeline).
    {
        srtp_t verify_ctx = create_srtp_ctx(keys.server_write_key, keys.server_write_salt, false);
        if (!verify_ctx)
        {
            LOG_ERROR(TAG, "SELFTEST ufrag=%s: could not build verify context", ice_ufrag.c_str());
        }
        else
        {
            uint8_t buf[64];
            memset(buf, 0, sizeof(buf));
            buf[0] = 0x80;
            buf[1] = 0x66; // marker=0, pt=102
            buf[2] = 0x00; buf[3] = 0x01; // seq=1
            buf[4] = 0x00; buf[5] = 0x00; buf[6] = 0x00; buf[7] = 0x00; // ts=0
            buf[8] = 0x12; buf[9] = 0x34; buf[10] = 0x56; buf[11] = 0x78; // ssrc
            const char *payload = "SELFTEST_PAYLOAD_1234";
            size_t payload_len = strlen(payload);
            memcpy(buf + 12, payload, payload_len);
            int len = static_cast<int>(12 + payload_len);

            srtp_err_status_t protect_status = srtp_protect(session->send_ctx, buf, &len);
            if (protect_status != srtp_err_status_ok)
            {
                LOG_ERROR(TAG, "SELFTEST ufrag=%s: srtp_protect failed status=%d", ice_ufrag.c_str(), static_cast<int>(protect_status));
            }
            else
            {
                srtp_err_status_t unprotect_status = srtp_unprotect(verify_ctx, buf, &len);
                if (unprotect_status != srtp_err_status_ok)
                {
                    LOG_ERROR(TAG, "SELFTEST ufrag=%s: FAIL - srtp_unprotect status=%d (round-trip broken)",
                              ice_ufrag.c_str(), static_cast<int>(unprotect_status));
                }
                else if (static_cast<size_t>(len) == 12 + payload_len &&
                         memcmp(buf + 12, payload, payload_len) == 0)
                {
                    LOG_INFO(TAG, "SELFTEST ufrag=%s: PASS - round-trip plaintext matches", ice_ufrag.c_str());
                }
                else
                {
                    LOG_ERROR(TAG, "SELFTEST ufrag=%s: FAIL - round-trip plaintext MISMATCH (len=%d)", ice_ufrag.c_str(), len);
                }
            }

            srtp_dealloc(verify_ctx);
        }
    }

    return 0;
}

void srtp_session_destroy(const std::string &ice_ufrag)
{
    pthread_mutex_lock(&g_sessions_lock);

    auto it = g_sessions.find(ice_ufrag);
    if (it != g_sessions.end())
    {
        if (it->second->send_ctx) srtp_dealloc(it->second->send_ctx);
        if (it->second->recv_ctx) srtp_dealloc(it->second->recv_ctx);
        delete it->second;
        g_sessions.erase(it);
    }

    pthread_mutex_unlock(&g_sessions_lock);
}

bool srtp_session_protect_rtp(const std::string &ice_ufrag, uint8_t *buffer, size_t buffer_capacity, int *len)
{
    pthread_mutex_lock(&g_sessions_lock);
    auto it = g_sessions.find(ice_ufrag);
    srtp_t ctx = (it != g_sessions.end()) ? it->second->send_ctx : nullptr;
    pthread_mutex_unlock(&g_sessions_lock);

    if (!ctx)
    {
        return false;
    }

    // srtp_protect() writes up to SRTP_MAX_TRAILER_LEN extra bytes
    // past the original packet (the auth tag) - refuse rather than
    // let libsrtp2 write past a buffer the caller didn't size for that.
    if (static_cast<size_t>(*len) + SRTP_MAX_TRAILER_LEN > buffer_capacity)
    {
        LOG_ERROR(TAG, "buffer too small to protect RTP for ufrag=%s (len=%d, capacity=%zu, need %d spare)",
                   ice_ufrag.c_str(), *len, buffer_capacity, SRTP_MAX_TRAILER_LEN);
        return false;
    }

    srtp_err_status_t status = srtp_protect(ctx, buffer, len);
    if (status != srtp_err_status_ok)
    {
        LOG_WARN(TAG, "srtp_protect() failed for ufrag=%s (status=%d)", ice_ufrag.c_str(), static_cast<int>(status));
        return false;
    }

    return true;
}

// TEMP DIAGNOSTIC (remove after root cause found): given the ACTUAL
// ciphertext this session just sent (post srtp_protect), tries to
// unprotect a copy of it using a freshly-built context keyed with the
// SAME server_write key/salt the browser itself would use to decrypt
// our outbound traffic - i.e. simulates the browser's decrypt step
// for this exact real packet, not a synthetic stand-in.
void srtp_session_debug_verify_roundtrip(const std::string &ice_ufrag, const uint8_t *ciphertext, size_t len)
{
    pthread_mutex_lock(&g_sessions_lock);
    auto it = g_sessions.find(ice_ufrag);
    if (it == g_sessions.end())
    {
        pthread_mutex_unlock(&g_sessions_lock);
        return;
    }
    std::vector<uint8_t> key = it->second->debug_server_write_key;
    std::vector<uint8_t> salt = it->second->debug_server_write_salt;
    pthread_mutex_unlock(&g_sessions_lock);

    srtp_t verify_ctx = create_srtp_ctx(key, salt, false);
    if (!verify_ctx)
    {
        LOG_ERROR(TAG, "DBGVERIFY ufrag=%s: could not build verify context", ice_ufrag.c_str());
        return;
    }

    std::vector<uint8_t> copy(ciphertext, ciphertext + len);
    int copy_len = static_cast<int>(len);

    srtp_err_status_t status = srtp_unprotect(verify_ctx, copy.data(), &copy_len);
    if (status != srtp_err_status_ok)
    {
        LOG_ERROR(TAG, "DBGVERIFY ufrag=%s: FAIL status=%d on REAL packet (len=%zu)",
                  ice_ufrag.c_str(), static_cast<int>(status), len);
    }
    else
    {
        LOG_INFO(TAG, "DBGVERIFY ufrag=%s: PASS on REAL packet (ciphertext=%zu -> plaintext=%d)",
                 ice_ufrag.c_str(), len, copy_len);
    }

    srtp_dealloc(verify_ctx);
}

bool srtp_session_unprotect_rtcp(const std::string &ice_ufrag, uint8_t *buffer, size_t buffer_capacity, int *len)
{
    (void)buffer_capacity; // unprotect only ever shrinks the packet (removes the auth tag) - no spare-room check needed, unlike protect

    pthread_mutex_lock(&g_sessions_lock);
    auto it = g_sessions.find(ice_ufrag);
    srtp_t ctx = (it != g_sessions.end()) ? it->second->recv_ctx : nullptr;
    pthread_mutex_unlock(&g_sessions_lock);

    if (!ctx)
    {
        return false;
    }

    srtp_err_status_t status = srtp_unprotect_rtcp(ctx, buffer, len);
    if (status != srtp_err_status_ok)
    {
        // A failure here is either a corrupted/replayed packet
        // (libsrtp2's own replay window rejects those) or - genuinely
        // notable - authentication failure, meaning the packet wasn't
        // actually from the peer this session's keys came from.
        LOG_WARN(TAG, "srtp_unprotect_rtcp() failed for ufrag=%s (status=%d)", ice_ufrag.c_str(), static_cast<int>(status));
        return false;
    }

    return true;
}
