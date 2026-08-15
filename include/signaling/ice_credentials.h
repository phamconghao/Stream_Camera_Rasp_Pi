#ifndef __ICE_CREDENTIALS_H__
#define __ICE_CREDENTIALS_H__

#include <string>

/**
 * PHASE 22.2.4 (WebRTC): random ICE ufrag/pwd generation.
 *
 * Unlike the DTLS cert (dtls_cert.h - one for the whole process
 * lifetime), ICE credentials are generated FRESH per peer connection
 * / signaling session - RFC 8839 section 5.3 requires ice-ufrag/
 * ice-pwd to be unique enough to disambiguate concurrent ICE checks
 * from different peers, which a single process-wide value can't do
 * once there's more than one browser connected at once.
 *
 * Character set and lengths follow RFC 8839 section 5.3's ice-char
 * ABNF (ALPHA / DIGIT / "+" / "/") and its length recommendations:
 * ufrag >= 4 characters, pwd >= 22 characters.
 */

struct ice_credentials_t
{
    std::string ufrag;
    std::string pwd;
};

// Generates a fresh random ufrag (4 chars) + pwd (22 chars) pair using
// a cryptographically-irrelevant but adequately unpredictable PRNG -
// these values only need to be hard to GUESS during the brief ICE
// connectivity-check window, not cryptographically secure the way the
// DTLS keys derived later (Phase 22.4/22.5) do.
ice_credentials_t generate_ice_credentials(void);

#endif // __ICE_CREDENTIALS_H__
