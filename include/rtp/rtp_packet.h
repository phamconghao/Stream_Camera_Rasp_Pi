#ifndef __RTP_PACKET_H__
#define __RTP_PACKET_H__

#include <stdint.h>
#include <stddef.h>

/**
 * ============================================================================
 * PIPELINE STAGE: RTP Packetizer -> RTP Packet Pool/Queue -> UDP Sender Thread
 * ============================================================================
 *
 * rtp_packet_t holds ONE fully-formed RTP packet (12-byte RTP header
 * already built + H.264 payload), exactly as it will go out on the
 * wire. `data`/`size` is what udp_sender_thread hands straight to
 * sendto() - no further transformation needed by the network stage.
 *
 * The other fields (timestamp/ssrc/sequence_number/payload_type/
 * nal_type/marker) mirror what's already encoded inside `data`'s RTP
 * header; they're kept alongside as plain fields purely for convenient
 * logging/debugging (e.g. udp_sender_thread logs sequence_number
 * without having to re-parse the header bytes).
 */

#define RTP_HEADER_SIZE 12 // fixed-size RTP header (RFC 3550): version/padding/marker/PT byte, seq, timestamp, SSRC - no CSRC list used here

// 1400 bytes keeps the total packet comfortably under a typical 1500-byte
// Ethernet MTU once RTP header + IP/UDP headers are added, avoiding IP
// fragmentation. NAL units larger than this need FU-A fragmentation
// (not yet implemented - see rtp_packetizer.cpp).
#define RTP_MAX_PAYLOAD_SIZE 1200
#define RTP_MAX_PACKET_SIZE (RTP_HEADER_SIZE + RTP_MAX_PAYLOAD_SIZE)

typedef struct
{
    uint32_t timestamp;         // RTP timestamp (90kHz clock for H.264, see RTP_CLOCK in rtp_packetizer.cpp) - also embedded in data[4..7]
    uint32_t ssrc;               // Synchronization Source identifier, constant for this stream - also embedded in data[8..11]
    size_t size;                 // total bytes filled in `data` (RTP_HEADER_SIZE + payload length)
    uint16_t sequence_number;     // RTP sequence number, increments by 1 per packet - also embedded in data[2..3]
    uint8_t data[RTP_MAX_PACKET_SIZE]; // the actual bytes to send: RTP header followed immediately by the H.264 payload
    uint8_t payload_type;        // RTP payload type field (96 = dynamic, used for H.264 here) - also embedded in data[1]
    uint8_t nal_type;            // NAL type this packet's payload came from (for logging only, not part of the wire format)
    bool marker;                 // RTP marker bit: true on the last packet of an access unit, tells the receiver "frame boundary here"
} rtp_packet_t;

#endif // __RTP_PACKET_H__
