#ifndef __JSON_LITE_H__
#define __JSON_LITE_H__

#include <string>
#include <map>
#include <vector>
#include <utility>

/**
 * Minimal, flat JSON object parser/builder - not a general JSON
 * library. Handles a single top-level object whose values are
 * strings, numbers, or booleans/null (kept as their literal text) -
 * no nested objects/arrays. Consistent with this project's hand-rolled
 * parsing elsewhere (e.g. rtsp_message.cpp) rather than pulling in a
 * library for a simple, fixed message shape.
 *
 * Used for signaling messages such as
 *   {"type":"offer","sdp":"v=0\r\no=-...\r\n..."}
 *   {"type":"ice-candidate","candidate":"...","sdpMid":"0","sdpMLineIndex":0}
 * - flat objects whose string values may contain escaped newlines.
 */

// Parses a single flat JSON object. String values are unescaped
// (\", \\, \n, \r, \t, \/ handled; \uXXXX passed through as literal
// text rather than decoded to keep this small). Numbers/true/false/
// null are kept as their literal source text. Returns an empty map if
// `json` isn't a well-formed flat object.
std::map<std::string, std::string> json_parse_object(const std::string &json);

// Builds a flat JSON object from ordered key/value pairs, escaping
// each value as a JSON string. Order is preserved for readability.
std::string json_build_object(const std::vector<std::pair<std::string, std::string>> &fields);

// Builds a JSON array of flat objects - "[{...},{...}]" - by calling
// json_build_object() on each inner vector and joining with ",". The
// one place this project needs anything beyond a single flat object
// (the admin dashboard's per-viewer session list, see
// webrtc_session_stats.h) - stays a thin wrapper rather than growing
// this file into a general nested-value JSON library.
std::string json_build_array_of_objects(const std::vector<std::vector<std::pair<std::string, std::string>>> &objects);

#endif // __JSON_LITE_H__
