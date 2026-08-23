#include "webrtc_sdp.h"

#include <sstream>
#include <set>

#include "base64.h"

namespace
{

// SDP lines are "\r\n"-separated "<type>=<value>" pairs (RFC 4566
// section 5). Browsers always send "\r\n", but tolerate a bare "\n"
// too in case something along the way normalizes line endings -
// costs nothing to accept both.
std::string strip_line_ending(const std::string &line)
{
    std::string out = line;
    while (!out.empty() && (out.back() == '\r' || out.back() == '\n'))
    {
        out.pop_back();
    }
    return out;
}

// Returns the substring after `prefix` if `line` starts with it,
// otherwise an empty optional-style pair (found=false).
bool try_strip_prefix(const std::string &line, const std::string &prefix, std::string &out_rest)
{
    if (line.size() < prefix.size() || line.compare(0, prefix.size(), prefix) != 0)
    {
        return false;
    }
    out_rest = line.substr(prefix.size());
    return true;
}

// Parses the payload type number out of an "a=rtpmap:<pt> <rest>" line
// (already stripped of the "a=rtpmap:" prefix, so `rest` starts with
// the PT number). Returns -1 on a malformed line.
int parse_pt_prefix(const std::string &rest, std::string &out_after_pt)
{
    size_t space = rest.find(' ');
    if (space == std::string::npos)
    {
        return -1;
    }

    try
    {
        int pt = std::stoi(rest.substr(0, space));
        out_after_pt = rest.substr(space + 1);
        return pt;
    }
    catch (...)
    {
        return -1;
    }
}

// Same profile-level-id extraction as rtsp_server.cpp's
// sps_profile_level_id_hex() - duplicated here rather than shared
// because the two SDP builders otherwise share nothing and pulling
// this one helper into a common header isn't worth the indirection
// for 6 lines of code.
static std::string sps_profile_level_id_hex(const std::vector<uint8_t> &sps)
{
    static const char *HEX = "0123456789ABCDEF";

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

} // namespace

webrtc_sdp_offer_t parse_webrtc_sdp_offer(const std::string &sdp)
{
    webrtc_sdp_offer_t offer;

    std::istringstream stream(sdp);
    std::string raw_line;

    bool in_video_section = false;

    // Every PT the offer declared as "H264/90000" via a=rtpmap, in the
    // order encountered - a=fmtp for that PT (with packetization-mode=1)
    // always appears later in the same media section, so this is
    // checked against as fmtp lines are seen below.
    std::set<int> h264_candidate_pts;

    while (std::getline(stream, raw_line))
    {
        std::string line = strip_line_ending(raw_line);
        std::string rest;

        // "m=video ..." starts the video media section - "a=mid"
        // appears once per m= section, so this project (video-only
        // for now) only cares about the mid inside the video section,
        // not any audio section that might precede or follow it.
        if (try_strip_prefix(line, "m=video", rest))
        {
            in_video_section = true;
            continue;
        }
        if (!line.empty() && line[0] == 'm' && line[1] == '=')
        {
            in_video_section = false; // entered a different (e.g. audio) m= section
            continue;
        }

        if (try_strip_prefix(line, "a=ice-ufrag:", rest))
        {
            offer.ice_ufrag = rest;
        }
        else if (try_strip_prefix(line, "a=ice-pwd:", rest))
        {
            offer.ice_pwd = rest;
        }
        else if (try_strip_prefix(line, "a=fingerprint:", rest))
        {
            // "<algo> <hex>", e.g. "sha-256 AB:CD:EF:..."
            size_t space = rest.find(' ');
            if (space != std::string::npos)
            {
                offer.fingerprint_algo = rest.substr(0, space);
                offer.fingerprint_hex = rest.substr(space + 1);
            }
        }
        else if (in_video_section && try_strip_prefix(line, "a=mid:", rest))
        {
            offer.mid = rest;
        }
        else if (in_video_section && try_strip_prefix(line, "a=rtpmap:", rest))
        {
            std::string codec;
            int pt = parse_pt_prefix(rest, codec);
            if (pt >= 0 && codec.rfind("H264/", 0) == 0)
            {
                h264_candidate_pts.insert(pt);
            }
        }
        else if (in_video_section && offer.h264_payload_type == 0 && try_strip_prefix(line, "a=fmtp:", rest))
        {
            std::string params;
            int pt = parse_pt_prefix(rest, params);

            // Only a PT already confirmed to be H264 (via its rtpmap
            // line, seen above - rtpmap always precedes fmtp for the
            // same PT in a well-formed offer) and explicitly declaring
            // packetization-mode=1 is usable: this project's packetizer
            // only ever produces single-NAL and FU-A (RFC 6184 5.6/5.8)
            // packets, both of which require mode 1. A mode-0 entry
            // (STAP-A/single-NAL only, no fragmentation) would silently
            // fail exactly like the PT mismatch bug this fixes - the
            // browser would reject or misinterpret FU-A packets sent
            // under a codec entry that never declared support for them.
            if (pt >= 0 && h264_candidate_pts.count(pt) &&
                params.find("packetization-mode=1") != std::string::npos)
            {
                offer.h264_payload_type = static_cast<uint8_t>(pt);
            }
        }
    }

    // ice-ufrag/ice-pwd/fingerprint are sometimes carried at the
    // session level (before any m= line) rather than repeated inside
    // each media section (RFC 8842) - the loop above already picks
    // those up regardless of in_video_section, since only the mid
    // check is gated on being inside the video section specifically.
    offer.valid = !offer.ice_ufrag.empty() &&
                  !offer.ice_pwd.empty() &&
                  !offer.fingerprint_algo.empty() &&
                  !offer.fingerprint_hex.empty() &&
                  !offer.mid.empty() &&
                  offer.h264_payload_type != 0;

    return offer;
}

std::string build_webrtc_sdp_answer(
    const std::string &ice_ufrag,
    const std::string &ice_pwd,
    const std::string &fingerprint_sha256,
    const std::string &mid,
    uint8_t h264_payload_type,
    const std::vector<uint8_t> &sps,
    const std::vector<uint8_t> &pps)
{
    std::string profile_level_id = sps_profile_level_id_hex(sps);
    std::string sprop_parameter_sets = base64_encode(sps) + "," + base64_encode(pps);
    std::string pt = std::to_string(h264_payload_type);

    std::ostringstream sdp;
    sdp << "v=0\r\n";
    sdp << "o=- 0 0 IN IP4 0.0.0.0\r\n";
    sdp << "s=Raspberry Pi Camera Stream\r\n";
    sdp << "t=0 0\r\n";
    sdp << "a=group:BUNDLE " << mid << "\r\n";
    sdp << "m=video 9 UDP/TLS/RTP/SAVPF " << pt << "\r\n";
    sdp << "c=IN IP4 0.0.0.0\r\n";
    sdp << "a=rtcp-mux\r\n";
    sdp << "a=mid:" << mid << "\r\n";
    sdp << "a=ice-ufrag:" << ice_ufrag << "\r\n";
    sdp << "a=ice-pwd:" << ice_pwd << "\r\n";
    sdp << "a=fingerprint:sha-256 " << fingerprint_sha256 << "\r\n";
    // Always "passive": this side's answer never initiates the DTLS
    // handshake itself - the browser always does, regardless of which
    // of "actpass"/"active" it offered.
    sdp << "a=setup:passive\r\n";
    sdp << "a=sendonly\r\n"; // this project only sends video, never receives any from the browser
    sdp << "a=rtpmap:" << pt << " H264/90000\r\n";
    sdp << "a=rtcp-fb:" << pt << " nack\r\n";
    sdp << "a=rtcp-fb:" << pt << " nack pli\r\n";
    sdp << "a=fmtp:" << pt << " packetization-mode=1;profile-level-id=" << profile_level_id
        << ";sprop-parameter-sets=" << sprop_parameter_sets << "\r\n";

    return sdp.str();
}
