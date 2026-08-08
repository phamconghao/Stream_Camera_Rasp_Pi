#ifndef __RTSP_MESSAGE_H__
#define __RTSP_MESSAGE_H__

#include <string>
#include <map>

/**
 * ============================================================================
 * PHASE 20: RTSP (RFC 2326) - request/response wire format
 * ============================================================================
 *
 * RTSP's message syntax deliberately mirrors HTTP/1.1: a request line,
 * CRLF-terminated headers, a blank line, then an optional body sized by
 * Content-Length. This file only knows that syntax - it has no opinion
 * about what OPTIONS/DESCRIBE/SETUP/PLAY/TEARDOWN actually DO (that's
 * rtsp_server.cpp's job, using rtsp_session_registry for state).
 *
 * Example request this parses:
 *   SETUP rtsp://192.168.1.50:8554/stream RTSP/1.0\r\n
 *   CSeq: 2\r\n
 *   Transport: RTP/AVP;unicast;client_port=5004-5005\r\n
 *   \r\n
 */

struct rtsp_request_t
{
    std::string method;  // "OPTIONS", "DESCRIBE", "SETUP", "PLAY", "TEARDOWN", ...
    std::string uri;     // e.g. "rtsp://192.168.1.50:8554/stream"
    std::string version; // "RTSP/1.0"
    std::map<std::string, std::string> headers; // header name -> value, as given (lookup is case-sensitive - see rtsp_header_get)
    std::string body;
};

struct rtsp_response_t
{
    int status_code = 200;
    std::string status_text = "OK";
    std::map<std::string, std::string> headers;
    std::string body;
};

/**
 * Parses one RTSP request out of `raw` (the request line + headers +
 * blank line + body, exactly as received off the socket - see
 * rtsp_server.cpp for how a full request is assembled from a stream of
 * recv() calls). Returns false if the request line or header block is
 * malformed; a missing/absent body is fine (most requests have none).
 */
bool rtsp_parse_request(const std::string &raw, rtsp_request_t *out);

/**
 * Case-insensitive header lookup (RTSP/HTTP header names are not
 * case-sensitive per the RFC, even though this project stores them
 * as-received in the map for simplicity). Returns "" if not present.
 */
std::string rtsp_header_get(const rtsp_request_t &req, const std::string &name);

/**
 * Serializes a response back into wire format (status line + headers +
 * blank line + body), setting Content-Length automatically from
 * resp.body. CSeq must already be set by the caller (rtsp_server.cpp)
 * to echo back the request's own CSeq, per RFC 2326 section 10.5.1.
 */
std::string rtsp_build_response(const rtsp_response_t &resp);

#endif // __RTSP_MESSAGE_H__
