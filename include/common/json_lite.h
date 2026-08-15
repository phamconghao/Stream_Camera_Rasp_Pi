#ifndef __JSON_LITE_H__
#define __JSON_LITE_H__

#include <string>
#include <map>
#include <vector>
#include <utility>

/**
 * Minimal, flat JSON object parser/builder - NOT a general JSON
 * library. Handles exactly what Phase 22's signaling messages need:
 * a single top-level object whose values are strings, numbers, or
 * booleans/null (kept as their literal text) - no nested
 * objects/arrays. Consistent with this project's "custom
 * implementation over library dependency" approach (same reasoning
 * as rtsp_message.cpp hand-parsing RTSP instead of pulling in a
 * library for it).
 *
 * PHASE 22.1 (signaling): messages exchanged over the WebSocket
 * signaling connection look like
 *   {"type":"offer","sdp":"v=0\r\no=-...\r\n..."}
 *   {"type":"ice-candidate","candidate":"...","sdpMid":"0","sdpMLineIndex":0}
 * - flat objects, string values may contain escaped newlines (SDP is
 * itself CRLF-separated text) - exactly the case this parser/builder
 * is built for.
 */

// Parses a single flat JSON object. String values are unescaped
// (\", \\, \n, \r, \t, \/ handled; \uXXXX passed through as literal
// text rather than decoded to keep this small). Numbers/true/false/
// null are kept as their literal source text (caller re-parses if it
// needs the actual number/bool). Returns an empty map if `json` isn't
// a well-formed flat object.
std::map<std::string, std::string> json_parse_object(const std::string &json);

// Builds a flat JSON object from ordered key/value pairs, escaping
// each value as a JSON string. Order is preserved (map alone doesn't
// guarantee the order a caller listed keys in) since some consumers
// may expect a particular field order for readability while debugging.
std::string json_build_object(const std::vector<std::pair<std::string, std::string>> &fields);

#endif // __JSON_LITE_H__
