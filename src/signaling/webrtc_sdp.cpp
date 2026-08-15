#include "webrtc_sdp.h"

#include <sstream>

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

} // namespace

webrtc_sdp_offer_t parse_webrtc_sdp_offer(const std::string &sdp)
{
    webrtc_sdp_offer_t offer;

    std::istringstream stream(sdp);
    std::string raw_line;

    bool in_video_section = false;

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
                  !offer.mid.empty();

    return offer;
}
