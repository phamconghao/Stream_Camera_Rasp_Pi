#ifndef __WEBRTC_SDP_H__
#define __WEBRTC_SDP_H__

#include <cstdint>
#include <string>
#include <vector>

/**
 * PHASE 22.2 (WebRTC-compatible SDP) - part 1/2 (this file: types +
 * parser declarations; the answer builder is part 2, not in this
 * file yet).
 *
 * Deliberately NOT the same struct/parser as rtsp/rtsp_message.h's
 * SDP handling from Phase 20 - WebRTC's offer/answer carries a
 * different set of attributes (ICE credentials, a DTLS fingerprint,
 * RTCP feedback types) that plain RTSP/RTP SDP never needed. Only
 * `sprop-parameter-sets`/SPS-PPS handling (parser/sps_pps_cache.h,
 * common/base64.h) is shared between the two.
 */

// Fields pulled out of a browser's SDP offer that the answer-building
// step (part 2/2) needs. Only WHAT this project's WebRTC path
// actually reads - not a general-purpose SDP model. Anything not
// listed here (audio m= lines, extra codecs, extmap, etc.) is
// ignored by the parser, not an error.
struct webrtc_sdp_offer_t
{
    std::string ice_ufrag;    // from "a=ice-ufrag:<value>"
    std::string ice_pwd;      // from "a=ice-pwd:<value>"
    std::string fingerprint_algo;  // e.g. "sha-256", from "a=fingerprint:<algo> <hex>"
    std::string fingerprint_hex;   // colon-separated hex, e.g. "AB:CD:EF:..."
    std::string mid;          // from the video m= section's "a=mid:<value>"

    // True once every field above required to build an answer was
    // actually found in the offer - parse_webrtc_sdp_offer() sets
    // this rather than the caller having to check each field for
    // emptiness individually.
    bool valid = false;
};

// Parses a browser-generated SDP offer (as delivered over the
// signaling WebSocket - see signaling/signaling_server.h) and
// extracts the fields webrtc_sdp_offer_t needs. Malformed input or a
// missing required attribute results in a struct with valid=false
// rather than a thrown exception - same "fail soft, let the caller
// decide what to do" convention as rtsp_message.h's rtsp_parse_request().
webrtc_sdp_offer_t parse_webrtc_sdp_offer(const std::string &sdp);

/**
 * PHASE 22.2 part 2/2: SDP answer builder.
 *
 * Builds the SDP this project sends back after receiving+parsing a
 * browser's offer (parse_webrtc_sdp_offer() above). Distinct from
 * Phase 20 step 5's handle_describe() SDP in the attributes that
 * matter for WebRTC specifically:
 *   - m=video ... UDP/TLS/RTP/SAVPF 96   (Secure + feedback profile,
 *     not RTSP's plain "RTP/AVP")
 *   - a=ice-ufrag / a=ice-pwd            (this project's OWN ICE
 *     credentials for THIS connection - see ice_credentials.h -  not
 *     an echo of the offer's)
 *   - a=fingerprint:sha-256 ...          (this project's DTLS cert
 *     fingerprint - see dtls_cert.h - proves which cert will actually
 *     be used in the Phase 22.4 DTLS handshake)
 *   - a=setup:passive                    (this project always waits
 *     for the browser to initiate the DTLS handshake, never the other
 *     way around - simplest role split for an embedded device that's
 *     always the "server" side of the connection)
 *   - a=rtcp-fb:96 nack / nack pli       (declares support for the
 *     browser's loss-recovery feedback - Phase 22.6's job to actually
 *     act on it, this just advertises the capability)
 *
 * sprop-parameter-sets is built the exact same way as Phase 20 step
 * 5's handle_describe() (same sps_pps_cache.h + base64.h), just
 * copied into this different SDP shape rather than shared code, since
 * the two SDP builders diverge in almost everything else.
 */
std::string build_webrtc_sdp_answer(
    const std::string &ice_ufrag,
    const std::string &ice_pwd,
    const std::string &fingerprint_sha256,
    const std::string &mid,
    const std::vector<uint8_t> &sps,
    const std::vector<uint8_t> &pps);

#endif // __WEBRTC_SDP_H__
