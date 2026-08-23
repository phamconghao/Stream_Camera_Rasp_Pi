#include "dtls_cert.h"

#include <openssl/x509.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/err.h>
#include <cstdio>

#include "log.h"

static const char *TAG = "DTLS_CERT";

// Owned for the process lifetime - the DTLS handshake needs both the
// cert and its matching private key to authenticate as this
// fingerprint, and regenerating them would change the fingerprint out
// from under any SDP answer already sent.
static X509 *g_cert = nullptr;
static EVP_PKEY *g_pkey = nullptr;
static std::string g_fingerprint;

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

// prime256v1 (P-256): the EC curve every major browser's WebRTC stack
// already supports for DTLS - RSA would also work but a 256-bit EC
// key is both faster to generate and smaller on the wire, and there's
// no compatibility reason to prefer RSA here.
static EVP_PKEY *generate_key(void)
{
    EVP_PKEY_CTX *pctx = EVP_PKEY_CTX_new_id(EVP_PKEY_EC, nullptr);
    if (!pctx)
    {
        log_openssl_errors("EVP_PKEY_CTX_new_id");
        return nullptr;
    }

    EVP_PKEY *pkey = nullptr;

    if (EVP_PKEY_keygen_init(pctx) <= 0 ||
        EVP_PKEY_CTX_set_ec_paramgen_curve_nid(pctx, NID_X9_62_prime256v1) <= 0 ||
        EVP_PKEY_keygen(pctx, &pkey) <= 0)
    {
        log_openssl_errors("EC key generation");
        EVP_PKEY_CTX_free(pctx);
        return nullptr;
    }

    EVP_PKEY_CTX_free(pctx);
    return pkey;
}

// A self-signed cert (issuer == subject, signed by its own key) valid
// for 1 year - WebRTC's trust model (see dtls_cert.h) never checks
// this expiry against a CA, but SOME DTLS stacks reject an
// already-expired cert outright regardless of trust model, so a
// generous 1-year validity window avoids that entirely without
// needing to think about renewal for this project's purposes.
static X509 *generate_self_signed_cert(EVP_PKEY *pkey)
{
    X509 *cert = X509_new();
    if (!cert)
    {
        log_openssl_errors("X509_new");
        return nullptr;
    }

    X509_set_version(cert, 2); // X509v3
    ASN1_INTEGER_set(X509_get_serialNumber(cert), 1);

    X509_gmtime_adj(X509_getm_notBefore(cert), 0);
    X509_gmtime_adj(X509_getm_notAfter(cert), 60L * 60 * 24 * 365);

    X509_set_pubkey(cert, pkey);

    X509_NAME *name = X509_get_subject_name(cert);
    X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
                                reinterpret_cast<const unsigned char *>("Stream_Camera_Rasp_Pi"), -1, -1, 0);
    X509_set_issuer_name(cert, name); // self-signed: issuer == subject

    if (X509_sign(cert, pkey, EVP_sha256()) == 0)
    {
        log_openssl_errors("X509_sign");
        X509_free(cert);
        return nullptr;
    }

    return cert;
}

static std::string compute_fingerprint_sha256(X509 *cert)
{
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int digest_len = 0;

    if (X509_digest(cert, EVP_sha256(), digest, &digest_len) == 0)
    {
        log_openssl_errors("X509_digest");
        return "";
    }

    // SDP's a=fingerprint value format (RFC 8122 section 5): uppercase
    // hex byte pairs joined with ':'.
    std::string out;
    out.reserve(digest_len * 3 - 1);

    for (unsigned int i = 0; i < digest_len; i++)
    {
        char buf[4];
        snprintf(buf, sizeof(buf), "%02X", digest[i]);
        if (i > 0)
        {
            out += ':';
        }
        out += buf;
    }

    return out;
}

// Public wrapper - same logic, exposed for dtls_handshake.cpp to call
// on the remote peer's certificate, not just this project's own.
std::string dtls_cert_fingerprint_sha256(X509 *cert)
{
    return compute_fingerprint_sha256(cert);
}

int dtls_cert_init(void)
{
    g_pkey = generate_key();
    if (!g_pkey)
    {
        return -1;
    }

    g_cert = generate_self_signed_cert(g_pkey);
    if (!g_cert)
    {
        EVP_PKEY_free(g_pkey);
        g_pkey = nullptr;
        return -1;
    }

    g_fingerprint = compute_fingerprint_sha256(g_cert);
    if (g_fingerprint.empty())
    {
        X509_free(g_cert);
        EVP_PKEY_free(g_pkey);
        g_cert = nullptr;
        g_pkey = nullptr;
        return -1;
    }

    LOG_INFO(TAG, "generated self-signed DTLS cert, fingerprint sha-256 %s", g_fingerprint.c_str());

    return 0;
}

void dtls_cert_cleanup(void)
{
    if (g_cert)
    {
        X509_free(g_cert);
        g_cert = nullptr;
    }
    if (g_pkey)
    {
        EVP_PKEY_free(g_pkey);
        g_pkey = nullptr;
    }
    g_fingerprint.clear();
}

std::string dtls_cert_get_fingerprint_sha256(void)
{
    return g_fingerprint;
}

X509 *dtls_cert_get_cert(void)
{
    return g_cert;
}

EVP_PKEY *dtls_cert_get_pkey(void)
{
    return g_pkey;
}
