#ifndef __STUN_MESSAGE_H__
#define __STUN_MESSAGE_H__

#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>

/**
 * PHASE 22.3.2: minimal STUN (RFC 5389) message building/parsing -
 * just enough for ICE connectivity checks (RFC 8445 section 7), NOT a
 * general STUN library. Only Binding Request/Response, and only the
 * attributes ICE connectivity checks actually use:
 *   USERNAME, MESSAGE-INTEGRITY, FINGERPRINT, XOR-MAPPED-ADDRESS.
 *
 * MESSAGE-INTEGRITY uses HMAC-SHA1 (via OpenSSL - this project's
 * second use of it after dtls_cert.cpp's certificate generation) -
 * this one IS genuinely security-relevant (it's what proves a
 * connectivity check actually came from the peer that received this
 * project's ice_pwd over the signaling channel, not an off-path
 * attacker guessing UDP ports), unlike sha1.h's WebSocket-handshake
 * SHA-1 which is unkeyed and non-adversarial. FINGERPRINT uses CRC-32
 * (hand-rolled, like sha1.h - it's a demultiplexing checksum to tell
 * STUN packets apart from other UDP traffic on the same port, not a
 * security mechanism).
 */

enum stun_message_class_t : uint16_t
{
    STUN_BINDING_REQUEST = 0x0001,
    STUN_BINDING_RESPONSE_SUCCESS = 0x0101,
};

struct stun_parsed_message_t
{
    uint16_t message_type = 0;
    uint8_t transaction_id[12] = {0};
    std::string username; // only present/meaningful on a Binding Request
    bool has_message_integrity = false;
    bool has_fingerprint = false;
    bool valid = false; // header parsed, FINGERPRINT (if present) checksum matches
};

// Parses a raw UDP payload as a STUN message. Does NOT verify
// MESSAGE-INTEGRITY (that requires knowing which ice_pwd to check
// against, which depends on the USERNAME attribute this function
// extracts - see stun_verify_message_integrity() below, called
// separately once the caller has looked up the right pwd). DOES
// verify FINGERPRINT if present, since that requires no external
// key. valid=false on anything truncated/malformed/failing FINGERPRINT.
stun_parsed_message_t parse_stun_message(const uint8_t *data, size_t size);

// Re-checks MESSAGE-INTEGRITY on a raw STUN message using `ice_pwd` as
// the HMAC-SHA1 key (RFC 5389 section 15.4) - takes the raw bytes
// again (not stun_parsed_message_t) because the HMAC covers the
// message's raw bytes up to (not including) the MESSAGE-INTEGRITY
// attribute itself.
bool stun_verify_message_integrity(const uint8_t *data, size_t size, const std::string &ice_pwd);

// Builds a Binding Success Response (RFC 5389 section 7.3.1) to
// `request_transaction_id`, containing XOR-MAPPED-ADDRESS for
// (mapped_ip, mapped_port) - the address the request appeared to come
// from, which is what tells the requesting ICE agent "here is the
// address YOU are reachable at from my side" - plus MESSAGE-INTEGRITY
// (keyed with ice_pwd) and FINGERPRINT.
std::vector<uint8_t> build_stun_binding_response(
    const uint8_t transaction_id[12],
    const std::string &mapped_ip,
    uint16_t mapped_port,
    const std::string &ice_pwd);

// Builds a Binding Request (RFC 5389 section 7.1) carrying USERNAME
// ("<remote_ufrag>:<local_ufrag>", RFC 8445 section 7.1.1),
// MESSAGE-INTEGRITY (keyed with `ice_pwd` - the REMOTE peer's pwd,
// since that's whose credentials this check is targeting), and
// FINGERPRINT. `out_transaction_id` is filled with the randomly
// generated transaction ID so the caller can match it against the
// eventual response.
std::vector<uint8_t> build_stun_binding_request(
    const std::string &username,
    const std::string &ice_pwd,
    uint8_t out_transaction_id[12]);

// Builds a Binding Request per RFC 5389's basic "Binding" usage - for
// querying a PUBLIC STUN server about this project's own
// server-reflexive address (Phase 24.3's discovery step), not for ICE
// connectivity checks between peers. Deliberately carries NO USERNAME/
// MESSAGE-INTEGRITY: RFC 5389 section 10 only requires those when a
// STUN usage specifically calls for authentication (ICE connectivity
// checks do, via ice_pwd - see build_stun_binding_request() above;
// plain NAT-discovery Binding usage does not), and public STUN servers
// generally neither expect nor check them. FINGERPRINT is still
// included since it isn't authentication, just a demux checksum.
std::vector<uint8_t> build_stun_binding_request_no_auth(uint8_t out_transaction_id[12]);

// Extracts XOR-MAPPED-ADDRESS (RFC 5389 section 15.2) from a raw
// Binding Success Response - the address the STUN server says this
// request appeared to come FROM, i.e. this project's own address as
// seen from outside any NAT in front of it. IPv4 only. Returns false
// if the attribute is missing/malformed/not IPv4, or the message is
// too short/truncated.
bool parse_stun_xor_mapped_address(const uint8_t *data, size_t size, std::string &out_ip, uint16_t &out_port);

#endif // __STUN_MESSAGE_H__
