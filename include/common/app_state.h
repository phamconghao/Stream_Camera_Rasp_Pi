#ifndef __APP_STATE_H__
#define __APP_STATE_H__

#include <atomic>

/**
 * App-level "is the whole application still running" flag.
 *
 * Ownership rule: only main() (or a signal handler installed by main())
 * writes to this flag. Individual thread modules (encoder_thread,
 * rtp_packetizer_thread, etc.) must NOT set this to false in their own
 * _stop() functions — each thread owns its own local running flag
 * instead, so threads can be started/stopped independently (needed once
 * the RTSP server manages per-client sessions). This flag exists for
 * app-wide signals such as Ctrl+C/SIGINT, where main() wants a single
 * place to check "should I start shutting everything down".
 */
extern std::atomic<bool> g_running;

#endif // __APP_STATE_H__
