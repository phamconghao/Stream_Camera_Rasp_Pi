#ifndef __PIPELINE_CONTROLLER_H__
#define __PIPELINE_CONTROLLER_H__

#include <cstdint>

/**
 * ============================================================================
 * PHASE 20 step 3: Lazy pipeline start/stop, driven by RTSP session count
 * ============================================================================
 *
 * Wraps the capture->encode->packetize->send data pipeline with a
 * reference count: the FIRST client to PLAY (0->1 transition) actually
 * starts it; the LAST client to leave PLAYING (1->0, via TEARDOWN or
 * the orphan reaper - see rtsp_server.cpp) stops it. Nobody watching
 * means the Pi's camera/encoder aren't running, saving CPU/power.
 *
 * Split into two tiers, matching what's actually cheap to repeat:
 *   - pipeline_controller_init()/_cleanup(): hardware init (V4L2 device
 *     open, pool/queue allocation) - done ONCE at process startup/exit,
 *     never toggled per-client. Re-opening V4L2 devices on every
 *     PLAY/TEARDOWN would be slow and fragile.
 *   - pipeline_controller_ensure_running()/_release(): the actual
 *     thread start/stop (encoder_thread, camera streaming,
 *     rtp_packetizer_thread, udp_sender_thread) - cheap, safe to
 *     restart, this is what actually toggles per RTSP session.
 *
 * Callers (rtsp_server.cpp) MUST only call ensure_running() on a
 * genuine READY->PLAYING transition and release() on a genuine
 * PLAYING->(removed) transition - NOT on every PLAY/TEARDOWN request
 * received, since a client may legitimately send PLAY twice (resume)
 * or TEARDOWN a session that was never PLAYing. Miscounting either
 * direction leaves the ref count permanently wrong: too high means the
 * pipeline never stops even with zero real viewers; too low means
 * release() underflows toward stopping the pipeline while someone is
 * still watching. See rtsp_server.cpp's handle_play/handle_teardown/
 * reaper_thread_func for how this is enforced.
 *
 * PHASE 20 step 4: RTP fan-out to every PLAYING session now lands in
 * udp_sender (see udp_sender.h) - this module no longer owns a fixed
 * dest_ip/dest_port at all. rtsp_server.cpp's handle_play()/
 * handle_teardown() call udp_sender_add_dest()/udp_sender_remove_dest()
 * directly, independent of the ensure_running()/release() calls below.
 * This module still only controls WHEN the pipeline runs; WHO the data
 * goes to is entirely rtsp_server.cpp + udp_sender's concern now.
 */

int pipeline_controller_init(void);
void pipeline_controller_cleanup(void);

void pipeline_controller_ensure_running(void);
void pipeline_controller_release(void);

#endif // __PIPELINE_CONTROLLER_H__
