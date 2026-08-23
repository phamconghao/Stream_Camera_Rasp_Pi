#ifndef __WEBRTC_SDP_H__
#define __WEBRTC_SDP_H__

#include <cstdint>
#include <string>
#include <vector>

/**
 * SDP offer parsing and answer building for WebRTC. Separate from
 * rtsp/rtsp_message.h's SDP handling - WebRTC's offer/answer carries a
 * different set of attributes (ICE credentials, DTLS fingerprint,
 * RTCP feedback types) than plain RTSP/RTP SDP. Only sprop-parameter-
 * sets/SPS-PPS handling (parser/sps_pps_cache.h, common/base64.h) is
 * shared between the two.
 */

// Fields pulled out of a browser's SDP offer needed to build an
// answer - not a general-purpose SDP model. Anything not listed here
// (audio m= lines, extra codecs, extmap, etc.) is ignored by the
// parser, not an error.
struct webrtc_sdp_offer_t
{
    std::string ice_ufrag;    // from "a=ice-ufrag:<value>"
    std::string ice_pwd;      // from "a=ice-pwd:<value>"
    std::string fingerprint_algo;  // e.g. "sha-256", from "a=fingerprint:<algo> <hex>"
    std::string fingerprint_hex;   // colon-separated hex, e.g. "AB:CD:EF:..."
    std::string mid;          // from the video m= section's "a=mid:<value>"

    // True once every field above required to build an answer was
    // actually found in the offer.
    bool valid = false;
};

// Parses a browser-generated SDP offer and extracts the fields
// webrtc_sdp_offer_t needs. Malformed input or a missing required
// attribute results in a struct with valid=false rather than a thrown
// exception.
webrtc_sdp_offer_t parse_webrtc_sdp_offer(const std::string &sdp);

/**
 * Builds the SDP answer sent back after parsing a browser's offer.
 * Notable attributes:
 *   - m=video ... UDP/TLS/RTP/SAVPF 96   (secure + feedback profile)
 *   - a=ice-ufrag / a=ice-pwd            (this side's own ICE
 *     credentials for this connection - see ice_credentials.h)
 *   - a=fingerprint:sha-256 ...          (this side's DTLS cert
 *     fingerprint - see dtls_cert.h)
 *   - a=setup:passive                    (this side always waits for
 *     the peer to initiate the DTLS handshake)
 *   - a=rtcp-fb:96 nack / nack pli       (declares support for
 *     loss-recovery feedback)
 *
 * sprop-parameter-sets is built the same way as the RTSP DESCRIBE
 * path (sps_pps_cache.h + base64.h), just into a different SDP shape.
 */
std::string build_webrtc_sdp_answer(
    const std::string &ice_ufrag,
    const std::string &ice_pwd,
    const std::string &fingerprint_sha256,
    const std::string &mid,
    const std::vector<uint8_t> &sps,
    const std::vector<uint8_t> &pps);

#endif // __WEBRTC_SDP_H__
