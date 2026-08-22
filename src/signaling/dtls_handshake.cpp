#include "dtls_handshake.h"

#include <pthread.h>
#include <mutex>
#include <condition_variable>
#include <deque>
#include <map>
#include <atomic>
#include <cstring>

#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/srtp.h>

#include "dtls_cert.h"
#include "srtp_session.h"
#include "webrtc_media_registry.h"
#include "log.h"

static const char *TAG = "DTLS_HS";

// This project only ever needs ONE SRTP protection profile - the one
// every major browser offers first/supports universally. Phase 22.5
// will read back whichever profile actually got negotiated (via
// SSL_get_selected_srtp_profile()) rather than assuming this string
// was necessarily what was picked, but offering just this one keeps
// the negotiation trivial.
static const char *SRTP_PROFILES = "SRTP_AES128_CM_SHA1_80";

// RFC 5764 section 4.2's fixed label for the SRTP keying-material
// export - every DTLS-SRTP implementation uses this exact string.
static const char *SRTP_KEYING_MATERIAL_LABEL = "EXTRACTOR-dtls_srtp";

// 2 x (16-byte AES-128 key + 14-byte salt) = 60 bytes: one key+salt
// for each direction, per RFC 5764 section 4.2 - fixed by the
// SRTP_AES128_CM_SHA1_80 profile's parameters, not configurable.
static constexpr size_t SRTP_KEYING_MATERIAL_LEN = 60;

// How long a handshake thread waits for new data before treating it
// as "no packet arrived, check whether OpenSSL wants a retransmit"
// (DTLSv1_get_timeout()/_handle_timeout() - see the handshake loop
// below). Also bounds how many such empty waits happen before giving
// up entirely.
static constexpr int WAIT_FOR_PACKET_MS = 1000;
static constexpr int MAX_RETRANSMIT_ROUNDS = 10; // ~10s of silence total before this project gives up on a handshake

static void log_openssl_errors(const char *what)
{
    unsigned long err;
    while ((err = ERR_get_error()) != 0)
    {
        char buf[256];
        ERR_error_string_n(err, buf, sizeof(buf));
        LOG_ERROR(TAG, "%s: %s", what, buf);
    }
}

struct dtls_session_t
{
    std::string ice_ufrag;
    std::string remote_fingerprint_algo;
    std::string remote_fingerprint_hex;

    SSL *ssl = nullptr;
    BIO *read_bio = nullptr;  // network -> here (dtls_handshake_on_packet writes into this)
    BIO *write_bio = nullptr; // here -> network (the handshake thread reads out of this, sends via send_fn)
    std::function<void(const uint8_t *, size_t)> send_fn;

    pthread_t thread{};
    bool thread_started = false;

    std::mutex incoming_lock;
    std::condition_variable incoming_cv;
    std::deque<std::vector<uint8_t>> incoming;

    std::atomic<bool> connected{false};
    std::atomic<bool> failed{false};

    std::vector<uint8_t> srtp_keying_material;
};

static SSL_CTX *g_ssl_ctx = nullptr;

static std::mutex g_sessions_lock;
static std::map<std::string, dtls_session_t *> g_sessions; // ice_ufrag -> session

// This project doesn't validate the peer's certificate as a CHAIN
// (there is no CA - every WebRTC endpoint's cert is self-signed) - it
// accepts whatever certificate is presented here unconditionally, and
// instead checks that exact certificate's fingerprint against what
// the SDP offer promised AFTER the handshake completes (see
// verify_remote_fingerprint() below). This callback exists only
// because OpenSSL requires SOME verify callback when
// SSL_VERIFY_PEER is set; always returning 1 defers all trust
// decisions to the fingerprint check.
static int accept_any_cert_verify_callback(int preverify_ok, X509_STORE_CTX *ctx)
{
    (void)preverify_ok;
    (void)ctx;
    return 1;
}

int dtls_handshake_init(void)
{
    g_ssl_ctx = SSL_CTX_new(DTLS_server_method());
    if (!g_ssl_ctx)
    {
        log_openssl_errors("SSL_CTX_new(DTLS_server_method)");
        return -1;
    }

    // a=setup:passive in every SDP answer this project sends (Phase
    // 22.2.5) means the browser always initiates the handshake and
    // this project always responds - DTLS_server_method() above
    // matches that; there is no client-role code path anywhere in
    // this module.

    if (SSL_CTX_use_certificate(g_ssl_ctx, dtls_cert_get_cert()) <= 0 ||
        SSL_CTX_use_PrivateKey(g_ssl_ctx, dtls_cert_get_pkey()) <= 0)
    {
        log_openssl_errors("loading cert/key into SSL_CTX");
        SSL_CTX_free(g_ssl_ctx);
        g_ssl_ctx = nullptr;
        return -1;
    }

    SSL_CTX_set_verify(g_ssl_ctx, SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT, accept_any_cert_verify_callback);

    // RFC 5764: negotiate which SRTP protection profile both sides use
    // as PART of the DTLS handshake itself (an extension), rather than
    // as a separate step afterward.
    if (SSL_CTX_set_tlsext_use_srtp(g_ssl_ctx, SRTP_PROFILES) != 0)
    {
        log_openssl_errors("SSL_CTX_set_tlsext_use_srtp");
        SSL_CTX_free(g_ssl_ctx);
        g_ssl_ctx = nullptr;
        return -1;
    }

    LOG_INFO(TAG, "initialized DTLS server context (profiles: %s)", SRTP_PROFILES);

    return 0;
}

void dtls_handshake_cleanup(void)
{
    std::lock_guard<std::mutex> lock(g_sessions_lock);

    for (auto &pair : g_sessions)
    {
        dtls_session_t *session = pair.second;

        if (session->thread_started)
        {
            pthread_cancel(session->thread); // handshake threads block on a condvar wait with a timeout, never indefinitely - cancellation here is a safety net for process shutdown, not the normal exit path
            pthread_join(session->thread, nullptr);
        }

        if (session->ssl)
        {
            SSL_free(session->ssl); // also frees read_bio/write_bio, since SSL_set_bio() transferred ownership to the SSL object
        }

        srtp_session_destroy(pair.first);

        delete session;
    }

    g_sessions.clear();

    if (g_ssl_ctx)
    {
        SSL_CTX_free(g_ssl_ctx);
        g_ssl_ctx = nullptr;
    }
}

void dtls_handshake_register_session(
    const std::string &ice_ufrag,
    const std::string &remote_fingerprint_algo,
    const std::string &remote_fingerprint_hex)
{
    std::lock_guard<std::mutex> lock(g_sessions_lock);

    dtls_session_t *session = new dtls_session_t();
    session->ice_ufrag = ice_ufrag;
    session->remote_fingerprint_algo = remote_fingerprint_algo;
    session->remote_fingerprint_hex = remote_fingerprint_hex;

    g_sessions[ice_ufrag] = session;

    LOG_INFO(TAG, "registered session ufrag=%s (expecting remote fingerprint %s %s)",
             ice_ufrag.c_str(), remote_fingerprint_algo.c_str(), remote_fingerprint_hex.c_str());
}

void dtls_handshake_unregister_session(const std::string &ice_ufrag)
{
    std::lock_guard<std::mutex> lock(g_sessions_lock);

    auto it = g_sessions.find(ice_ufrag);
    if (it == g_sessions.end())
    {
        return;
    }

    dtls_session_t *session = it->second;

    if (session->thread_started)
    {
        pthread_cancel(session->thread);
        pthread_join(session->thread, nullptr);
    }

    if (session->ssl)
    {
        SSL_free(session->ssl);
    }

    srtp_session_destroy(ice_ufrag); // no-op if srtp_session_create() was never reached/succeeded for this session
    webrtc_media_registry_remove(ice_ufrag); // no-op if it was never added

    delete session;
    g_sessions.erase(it);
}

// Uppercase-normalizes before comparing - both this project's own
// fingerprints (dtls_cert_fingerprint_sha256()) and a browser's SDP
// offer fingerprint are conventionally uppercase already, but
// normalizing defensively costs nothing and avoids a false-negative
// "MITM detected" from a harmless case difference.
static bool fingerprints_match(const std::string &a, const std::string &b)
{
    if (a.size() != b.size())
    {
        return false;
    }

    for (size_t i = 0; i < a.size(); i++)
    {
        if (std::toupper(static_cast<unsigned char>(a[i])) != std::toupper(static_cast<unsigned char>(b[i])))
        {
            return false;
        }
    }

    return true;
}

// Drains whatever the SSL state machine has queued up in write_bio
// (a new handshake flight, an alert, a retransmission - anything DTLS
// decided to send as a result of the last SSL_do_handshake()/
// DTLSv1_handle_timeout() call) and hands each chunk to send_fn.
// BIO_read on a mem BIO returns everything currently buffered in one
// call for a well-formed DTLS record, so a single read+send per drain
// is sufficient here (unlike a stream socket, there's no partial-read
// concern).
static void drain_and_send(dtls_session_t *session)
{
    uint8_t buf[4096]; // generous for one DTLS handshake flight over UDP - larger than any single record this exchange produces
    int n;

    while ((n = BIO_read(session->write_bio, buf, sizeof(buf))) > 0)
    {
        session->send_fn(buf, static_cast<size_t>(n));
    }
}

// The per-session handshake thread body - runs SSL_do_handshake() in
// a loop, feeding it network bytes (queued by dtls_handshake_on_packet)
// and handling DTLS's own retransmission timers, until the handshake
// either completes or this project gives up (MAX_RETRANSMIT_ROUNDS of
// silence). Exits immediately on success/failure - no ongoing
// keepalive loop, since SRTP (Phase 22.5) is a separate concern that
// doesn't need this thread once the handshake itself is done.
static void *handshake_thread_func(void *arg)
{
    dtls_session_t *session = static_cast<dtls_session_t *>(arg);

    LOG_INFO(TAG, "handshake thread started for ufrag=%s", session->ice_ufrag.c_str());

    int silent_rounds = 0;

    while (true)
    {
        int ret = SSL_do_handshake(session->ssl);
        drain_and_send(session); // whatever this call produced (a flight, a retransmit, an alert) goes out regardless of ret's value

        if (ret == 1)
        {
            break; // handshake complete - verified below, outside this loop
        }

        int ssl_err = SSL_get_error(session->ssl, ret);
        if (ssl_err != SSL_ERROR_WANT_READ)
        {
            // A real handshake failure (bad cert, protocol mismatch,
            // alert from the peer) - not just "need more data".
            log_openssl_errors("SSL_do_handshake");
            session->failed = true;
            LOG_WARN(TAG, "handshake failed for ufrag=%s (SSL error %d)", session->ice_ufrag.c_str(), ssl_err);
            return nullptr;
        }

        // WANT_READ: wait for either new network bytes (pushed by
        // dtls_handshake_on_packet) or WAIT_FOR_PACKET_MS to elapse -
        // whichever comes first.
        std::unique_lock<std::mutex> lock(session->incoming_lock);
        bool got_packet = session->incoming_cv.wait_for(
            lock, std::chrono::milliseconds(WAIT_FOR_PACKET_MS),
            [session]() { return !session->incoming.empty(); });

        if (got_packet)
        {
            silent_rounds = 0;
            while (!session->incoming.empty())
            {
                std::vector<uint8_t> &packet = session->incoming.front();
                BIO_write(session->read_bio, packet.data(), static_cast<int>(packet.size()));
                session->incoming.pop_front();
            }
            lock.unlock();
            continue; // retry SSL_do_handshake() now that read_bio has data
        }

        lock.unlock();

        // Nothing arrived within the wait window - this is DTLS's
        // normal "the network might have dropped our last flight"
        // case, not an error by itself. RFC 6347 section 4.2.4's
        // retransmission timer: ask OpenSSL whether it thinks a
        // retransmit is due yet (DTLSv1_get_timeout() implements its
        // own backoff), and if so, trigger it.
        struct timeval timeout;
        if (DTLSv1_get_timeout(session->ssl, &timeout) == 1)
        {
            DTLSv1_handle_timeout(session->ssl); // re-queues the last flight into write_bio internally
            drain_and_send(session);
        }

        silent_rounds++;
        if (silent_rounds >= MAX_RETRANSMIT_ROUNDS)
        {
            session->failed = true;
            LOG_WARN(TAG, "handshake timed out for ufrag=%s after %d silent rounds (peer unreachable or gave up?)",
                     session->ice_ufrag.c_str(), MAX_RETRANSMIT_ROUNDS);
            return nullptr;
        }
    }

    // Handshake completed at the DTLS protocol level - now the actual
    // trust decision (see dtls_cert.h's header comment on WebRTC's
    // fingerprint-based trust model, and accept_any_cert_verify_callback()
    // above for why OpenSSL's own verify path was deliberately
    // permissive).
    X509 *peer_cert = SSL_get_peer_certificate(session->ssl);
    if (!peer_cert)
    {
        // SSL_VERIFY_FAIL_IF_NO_PEER_CERT should make this
        // unreachable (the handshake itself would have failed
        // earlier), but checking explicitly costs nothing and avoids
        // ever treating a null certificate as "trusted" by accident.
        session->failed = true;
        LOG_ERROR(TAG, "handshake for ufrag=%s completed but presented no certificate", session->ice_ufrag.c_str());
        return nullptr;
    }

    std::string actual_fingerprint = dtls_cert_fingerprint_sha256(peer_cert);
    X509_free(peer_cert);

    if (!fingerprints_match(actual_fingerprint, session->remote_fingerprint_hex))
    {
        // This is the check that actually matters for security here:
        // the cert presented during the handshake does NOT match what
        // was promised over signaling. Either a stale/wrong
        // credential, or someone else answering on this UDP path.
        session->failed = true;
        LOG_ERROR(TAG, "FINGERPRINT MISMATCH for ufrag=%s - expected %s, got %s (possible MITM or stale offer)",
                  session->ice_ufrag.c_str(), session->remote_fingerprint_hex.c_str(), actual_fingerprint.c_str());
        return nullptr;
    }

    // Phase 22.5 prep: export the keying material now, while the
    // handshake's cipher state is available, and cache it on the
    // session - re-deriving it later would need the same SSL* anyway,
    // so there's no reason to defer this to whenever Phase 22.5 first
    // asks for it.
    session->srtp_keying_material.resize(SRTP_KEYING_MATERIAL_LEN);
    int export_ok = SSL_export_keying_material(
        session->ssl,
        session->srtp_keying_material.data(), SRTP_KEYING_MATERIAL_LEN,
        SRTP_KEYING_MATERIAL_LABEL, strlen(SRTP_KEYING_MATERIAL_LABEL),
        nullptr, 0, // no extra context - RFC 5764 doesn't use one for this label
        0);

    if (export_ok != 1)
    {
        log_openssl_errors("SSL_export_keying_material");
        session->failed = true;
        LOG_ERROR(TAG, "failed to export SRTP keying material for ufrag=%s", session->ice_ufrag.c_str());
        return nullptr;
    }

    session->connected = true;
    LOG_INFO(TAG, "DTLS handshake COMPLETE for ufrag=%s - fingerprint verified, SRTP keying material exported",
             session->ice_ufrag.c_str());

    // Phase 22.5.3: the keying material is only useful once turned
    // into actual libsrtp2 contexts - do that immediately rather than
    // waiting for something else to notice this session connected,
    // since nothing else currently polls dtls_handshake_is_connected()
    // for that purpose.
    if (srtp_session_create(session->ice_ufrag, session->srtp_keying_material) < 0)
    {
        LOG_ERROR(TAG, "failed to create SRTP session for ufrag=%s despite a successful DTLS handshake",
                  session->ice_ufrag.c_str());
        // Not treated as a DTLS-level failure (session->connected stays
        // true - the handshake itself genuinely succeeded) - but this
        // session's traffic can't actually be encrypted/decrypted
        // until something retries srtp_session_create() for it.
    }
    else
    {
        // Phase 22.6.3: only mark this session "ready for media" once
        // its SRTP context genuinely exists - webrtc_sender_thread.cpp
        // (Phase 22.6.4) will start fanning real RTP out to it the
        // moment this call returns, so doing this before
        // srtp_session_create() succeeded would mean sends failing
        // for a session this registry claims is ready.
        webrtc_media_registry_add(session->ice_ufrag);
    }

    return nullptr;
}

void dtls_handshake_on_packet(
    const std::string &ice_ufrag,
    const uint8_t *data,
    size_t size,
    std::function<void(const uint8_t *, size_t)> send_fn)
{
    std::unique_lock<std::mutex> sessions_lock(g_sessions_lock);

    auto it = g_sessions.find(ice_ufrag);
    if (it == g_sessions.end())
    {
        LOG_WARN(TAG, "DTLS packet for unknown session ufrag=%s, ignoring", ice_ufrag.c_str());
        return;
    }

    dtls_session_t *session = it->second;

    if (!session->ssl)
    {
        // First DTLS packet for this session - this is where the
        // handshake actually begins (nothing before this point
        // touched OpenSSL for this session at all).
        session->ssl = SSL_new(g_ssl_ctx);
        session->read_bio = BIO_new(BIO_s_mem());
        session->write_bio = BIO_new(BIO_s_mem());

        // Makes an empty read_bio return "try again" (retryable, ->
        // SSL_ERROR_WANT_READ) instead of "EOF" - essential for using
        // a mem BIO as a non-blocking network stand-in the way this
        // module does; without this, OpenSSL would treat "no data
        // queued yet" as the peer having closed the connection.
        BIO_set_mem_eof_return(session->read_bio, -1);

        SSL_set_bio(session->ssl, session->read_bio, session->write_bio); // SSL now owns both BIOs
        SSL_set_accept_state(session->ssl); // server role - see dtls_handshake_init()'s a=setup:passive note

        session->send_fn = send_fn;

        if (pthread_create(&session->thread, nullptr, handshake_thread_func, session) != 0)
        {
            LOG_ERROR(TAG, "failed to spawn handshake thread for ufrag=%s", ice_ufrag.c_str());
            SSL_free(session->ssl);
            session->ssl = nullptr;
            sessions_lock.unlock();
            return;
        }

        session->thread_started = true;
    }

    sessions_lock.unlock(); // done touching g_sessions - the rest only touches this specific session's own lock

    {
        std::lock_guard<std::mutex> lock(session->incoming_lock);
        session->incoming.emplace_back(data, data + size);
    }
    session->incoming_cv.notify_one();
}

bool dtls_handshake_is_connected(const std::string &ice_ufrag)
{
    std::lock_guard<std::mutex> lock(g_sessions_lock);

    auto it = g_sessions.find(ice_ufrag);
    if (it == g_sessions.end())
    {
        return false;
    }

    return it->second->connected;
}

bool dtls_handshake_get_srtp_keying_material(const std::string &ice_ufrag, std::vector<uint8_t> &out)
{
    std::lock_guard<std::mutex> lock(g_sessions_lock);

    auto it = g_sessions.find(ice_ufrag);
    if (it == g_sessions.end() || !it->second->connected)
    {
        return false;
    }

    out = it->second->srtp_keying_material;
    return true;
}
