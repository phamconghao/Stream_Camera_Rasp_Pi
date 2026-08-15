#include "rtsp_message.h"

#include <sstream>
#include <algorithm>
#include <cctype>

/**
 * RTSP/1.0 request line + header parsing (RFC 2326 section 4), hand-
 * rolled the same way this project hand-rolls RTP/RTCP rather than
 * pulling in a library for a text protocol this simple. Used by
 * rtsp_server.cpp's connection_thread_func() to turn raw bytes read
 * off a TCP socket into a structured rtsp_request_t, and to turn a
 * handler's rtsp_response_t back into bytes to send.
 */

// Case-insensitive string comparison - RTSP header names are
// case-insensitive per RFC 2326 (same as HTTP), so a client sending
// "content-length" instead of "Content-Length" must still be found by
// rtsp_header_get() below.
static bool iequals(const std::string &a, const std::string &b)
{
    if (a.size() != b.size())
    {
        return false;
    }

    for (size_t i = 0; i < a.size(); i++)
    {
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i])))
        {
            return false;
        }
    }

    return true;
}

// Strips leading/trailing whitespace (space, tab, CR, LF) - used on
// every header line since RTSP allows (and some clients send) extra
// whitespace around the ':' separator that isn't semantically
// meaningful.
static std::string trim(const std::string &s)
{
    size_t start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos)
    {
        return "";
    }

    size_t end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

// Parses a raw RTSP request: request line ("METHOD uri VERSION"),
// followed by "Name: value" headers, terminated by a blank line, with
// anything after that blank line treated as the body (RTSP requests
// this project handles - OPTIONS/DESCRIBE/SETUP/PLAY/TEARDOWN - never
// actually have a body, but the split is done correctly regardless in
// case that ever changes). Returns false on anything that doesn't
// even have a well-formed request line - a missing/malformed header
// line is more forgiving (see the "continue" cases below), since one
// bad header shouldn't sink an otherwise-parseable request.
bool rtsp_parse_request(const std::string &raw, rtsp_request_t *out)
{
    if (!out)
    {
        return false;
    }

    // Accept both "\r\n\r\n" (the RFC-correct line ending) and a bare
    // "\n\n" - some non-browser RTSP clients/test tools normalize line
    // endings, and there's no cost to tolerating both.
    size_t header_end = raw.find("\r\n\r\n");
    size_t sep_len = 4;
    if (header_end == std::string::npos)
    {
        header_end = raw.find("\n\n");
        sep_len = 2;
        if (header_end == std::string::npos)
        {
            return false; // no blank-line terminator found at all - not a complete request yet
        }
    }

    std::string header_block = raw.substr(0, header_end);
    out->body = raw.substr(header_end + sep_len);

    std::istringstream stream(header_block);
    std::string line;

    if (!std::getline(stream, line))
    {
        return false; // empty header block - no request line to parse
    }

    line = trim(line);
    std::istringstream request_line(line);
    if (!(request_line >> out->method >> out->uri >> out->version))
    {
        return false; // request line must have exactly 3 whitespace-separated tokens
    }

    while (std::getline(stream, line))
    {
        line = trim(line);
        if (line.empty())
        {
            continue; // blank line inside the header block (shouldn't normally happen, harmless to skip)
        }

        size_t colon = line.find(':');
        if (colon == std::string::npos)
        {
            continue; // malformed header line (no ':') - skip it rather than failing the whole request
        }

        std::string name = trim(line.substr(0, colon));
        std::string value = trim(line.substr(colon + 1));
        out->headers[name] = value;
    }

    return true;
}

// Case-insensitive header lookup (see iequals() above) - returns ""
// if the header isn't present, since RTSP has no header that's ever
// meaningfully distinguished from "empty string" in this project's
// usage (callers check for a required header being present via a
// separate map lookup where that distinction actually matters, e.g.
// rtsp_server.cpp's handle_setup() checking for a Transport header).
std::string rtsp_header_get(const rtsp_request_t &req, const std::string &name)
{
    for (const auto &pair : req.headers)
    {
        if (iequals(pair.first, name))
        {
            return pair.second;
        }
    }

    return "";
}

// Serializes an rtsp_response_t back into the raw bytes to send on
// the wire: status line, then headers, then (if there's a body) an
// auto-computed Content-Length, then a blank line, then the body
// itself. Callers never need to set Content-Length manually - it's
// always derived from resp.body.size() here so it can never drift out
// of sync with the actual body being sent.
std::string rtsp_build_response(const rtsp_response_t &resp)
{
    std::ostringstream out;

    out << "RTSP/1.0 " << resp.status_code << " " << resp.status_text << "\r\n";

    for (const auto &pair : resp.headers)
    {
        out << pair.first << ": " << pair.second << "\r\n";
    }

    if (!resp.body.empty())
    {
        out << "Content-Length: " << resp.body.size() << "\r\n";
    }

    out << "\r\n";
    out << resp.body;

    return out.str();
}
