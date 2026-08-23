#ifndef __SPS_PPS_CACHE_H__
#define __SPS_PPS_CACHE_H__

#include <cstdint>
#include <cstddef>
#include <vector>

/**
 * Keeps the most recently seen SPS (NAL type 7) and PPS (NAL type 8)
 * around. The hardware encoder emits these periodically alongside
 * H.264 slice data (typically once per IDR/keyframe interval, not
 * just at startup) - see h264_nal_parser.h for how rtp_packetizer_thread
 * walks every NAL in each access unit. This cache lets a later
 * consumer (e.g. rtsp_server.cpp's handle_describe()) build a real SDP
 * `a=fmtp:96 ...;sprop-parameter-sets=...` line from actual encoder
 * output, without touching the hardware encoder or waiting for a
 * fresh frame.
 *
 * Ownership: this module owns its own copies - h264_nal_t::data points
 * into an encoded_frame_t that gets released and reused almost
 * immediately, so the cache must copy the bytes, not just remember the
 * pointer.
 *
 * Thread-safety: set_sps()/set_pps() are called from
 * rtp_packetizer_thread (one writer); get_sps()/get_pps() are called
 * from RTSP connection threads handling DESCRIBE (one or more
 * readers). Every function here takes an internal lock.
 */

void sps_pps_cache_init(void);
void sps_pps_cache_cleanup(void);

// Copies `data`/`size` in - safe to call with a pointer that's about
// to be invalidated (e.g. encoded_frame_pool_release() right after).
// Overwrites any previously cached SPS.
void sps_pps_cache_set_sps(const uint8_t *data, size_t size);
void sps_pps_cache_set_pps(const uint8_t *data, size_t size);

// Returns a copy of the most recently cached SPS/PPS bytes (NAL header
// byte included, Annex-B start code not included). Empty vector if
// nothing has been cached yet.
std::vector<uint8_t> sps_pps_cache_get_sps(void);
std::vector<uint8_t> sps_pps_cache_get_pps(void);

// True once at least one SPS and at least one PPS have been cached -
// the minimum needed to build a valid sprop-parameter-sets SDP
// attribute (RFC 6184 section 8.1).
bool sps_pps_cache_has_both(void);

#endif // __SPS_PPS_CACHE_H__
