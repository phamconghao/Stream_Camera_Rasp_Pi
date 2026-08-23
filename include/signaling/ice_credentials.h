#ifndef __ICE_CREDENTIALS_H__
#define __ICE_CREDENTIALS_H__

#include <string>

/**
 * Random ICE ufrag/pwd generation, per peer connection (not
 * process-wide, unlike the DTLS cert in dtls_cert.h). RFC 8839
 * section 5.3 requires ice-ufrag/ice-pwd to be unique enough to
 * disambiguate concurrent ICE checks from different peers.
 *
 * Character set and lengths follow RFC 8839 section 5.3's ice-char
 * ABNF (ALPHA / DIGIT / "+" / "/"): ufrag >= 4 characters, pwd >= 22
 * characters.
 */

struct ice_credentials_t
{
    std::string ufrag;
    std::string pwd;
};

// Generates a fresh random ufrag (4 chars) + pwd (22 chars) pair.
// These values only need to be hard to guess during the brief ICE
// connectivity-check window, not cryptographically secure the way the
// DTLS-derived keys are.
ice_credentials_t generate_ice_credentials(void);

#endif // __ICE_CREDENTIALS_H__
