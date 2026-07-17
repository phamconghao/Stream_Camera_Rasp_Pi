#ifndef __RTP_PACKETIZER_THREAD_H__
#define __RTP_PACKETIZER_THREAD_H__

/**
 * PIPELINE STAGE (thread wrapper): Encoded Frame Queue -> [THIS] -> RTP Packet Pool/Queue
 *
 * Pops H.264 access units from encoded_frame_queue, splits each into
 * NAL units (h264_nal_parser), turns each NAL into an RTP packet
 * (rtp_packetizer), and pushes the result into rtp_packet_queue for
 * udp_sender_thread. See rtp_packetizer_thread.cpp for the full loop.
 */

// Spawn the RTP packetizer thread. Returns 0/-1.
int rtp_packetizer_thread_start(void);

// Signal the thread to stop, wake it if blocked on an empty
// encoded_frame_queue, and join it.
void rtp_packetizer_thread_stop(void);

#endif // __RTP_PACKETIZER_THREAD_H__
