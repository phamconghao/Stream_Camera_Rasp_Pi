#ifndef __SPS_PPS_CACHE_H__
#define __SPS_PPS_CACHE_H__

#include <cstdint>
#include <cstddef>
#include <vector>

/**
 * ============================================================================
 * PHASE 20 step 5 (part 1/2): SPS/PPS cache
 * ============================================================================
 *
 * The BCM2835 hardware encoder emits SPS (NAL type 7) and PPS (NAL type
 * 8) periodically alongside the H.264 slice data (typically once per
 * IDR/keyframe interval, not just once at startup) - see
 * h264_nal_parser.h for how rtp_packetizer_thread already walks every
 * NAL in each access unit. This module is the single place that keeps
 * the MOST RECENT copy of each around after they fly past on their way
 * to being RTP-packetized, so a later consumer - concretely,
 * rtsp_server.cpp's handle_describe() (Phase 20 step 5 part 2) - can
 * build a real SDP `a=fmtp:96 ...;sprop-parameter-sets=...` line from
 * actual encoder output instead of a hardcoded placeholder, without
 * needing to touch the hardware encoder or wait for a fresh frame
 * itself.
 *
 * Ownership/lifetime: this module OWNS its own copies (std::vector,
 * not a pointer into the caller's buffer) - h264_nal_t::data from
 * h264_nal_parser points into an encoded_frame_t that gets released
 * back to encoded_frame_pool and reused for the next frame almost
 * immediately, so the cache must copy the bytes, not just remember
 * the pointer.
 *
 * Thread-safety: sps_pps_cache_set_sps()/_set_pps() are called from
 * rtp_packetizer_thread (one writer); sps_pps_cache_get_sps()/_get_pps()
 * will be called from RTSP connection threads handling DESCRIBE (one
 * or more readers, one per concurrent RTSP connection - see
 * rtsp_server.cpp). Every function here takes an internal lock, same
 * convention as rtsp_session_registry.cpp.
 */

void sps_pps_cache_init(void);
void sps_pps_cache_cleanup(void);

// Copies `data`/`size` in - safe to call with a pointer that's about
// to be invalidated (e.g. encoded_frame_pool_release() right after).
// Overwrites any previously cached SPS.
void sps_pps_cache_set_sps(const uint8_t *data, size_t size);
void sps_pps_cache_set_pps(const uint8_t *data, size_t size);

// Returns a copy of the most recently cached SPS/PPS bytes (NAL header
// byte included, Annex-B start code NOT included - same convention as
// h264_nal_t::data). Empty vector if nothing has been cached yet (e.g.
// DESCRIBE arrives before the pipeline has ever run a single frame).
std::vector<uint8_t> sps_pps_cache_get_sps(void);
std::vector<uint8_t> sps_pps_cache_get_pps(void);

// True once at least one SPS AND at least one PPS have been cached -
// the minimum needed to build a valid sprop-parameter-sets SDP
// attribute (RFC 6184 section 8.1). handle_describe() (step 5 part 2)
// uses this to decide whether it has real parameter sets to offer yet.
bool sps_pps_cache_has_both(void);

#endif // __SPS_PPS_CACHE_H__
