#ifndef __BASE64_H__
#define __BASE64_H__

#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>

/**
 * Minimal standard base64 encoder (RFC 4648 alphabet, with padding).
 * No decoder - nothing in this codebase needs to decode base64 (yet).
 *
 * PHASE 20 step 5 (part 3/4): the SDP `a=fmtp:96 ...;sprop-parameter-
 * sets=<SPS-base64>,<PPS-base64>` attribute (RFC 6184 section 8.1)
 * requires the raw SPS/PPS NAL bytes (see sps_pps_cache.h) to be
 * base64-encoded - this is the one place that does that encoding.
 */
std::string base64_encode(const std::vector<uint8_t> &data);

#endif // __BASE64_H__
