#include "json_lite.h"

namespace
{

void skip_whitespace(const std::string &s, size_t &pos)
{
    while (pos < s.size() && (s[pos] == ' ' || s[pos] == '\t' || s[pos] == '\r' || s[pos] == '\n'))
    {
        pos++;
    }
}

// Parses a JSON string literal starting at s[pos] == '"'. Advances
// pos past the closing quote. Returns false on truncated/malformed
// input (caller bails out of the whole object parse in that case).
bool parse_string_literal(const std::string &s, size_t &pos, std::string &out)
{
    if (pos >= s.size() || s[pos] != '"')
    {
        return false;
    }
    pos++; // skip opening quote

    out.clear();

    while (pos < s.size() && s[pos] != '"')
    {
        if (s[pos] == '\\' && pos + 1 < s.size())
        {
            char esc = s[pos + 1];
            switch (esc)
            {
                case '"': out += '"'; break;
                case '\\': out += '\\'; break;
                case '/': out += '/'; break;
                case 'n': out += '\n'; break;
                case 'r': out += '\r'; break;
                case 't': out += '\t'; break;
                default:
                    // \uXXXX and any other escape we don't specially
                    // handle: keep the raw characters rather than
                    // trying to decode a codepoint - good enough for
                    // the ASCII-only fields (type/sdpMid/candidate)
                    // and SDP text this project actually sends.
                    out += '\\';
                    out += esc;
                    break;
            }
            pos += 2;
        }
        else
        {
            out += s[pos];
            pos++;
        }
    }

    if (pos >= s.size())
    {
        return false; // ran off the end without a closing quote
    }

    pos++; // skip closing quote
    return true;
}

// Parses a bare (unquoted) JSON value - number, true, false, or null -
// by just collecting characters up to the next ',' '}' or whitespace.
// Kept as literal text (see json_lite.h).
std::string parse_bare_literal(const std::string &s, size_t &pos)
{
    std::string out;
    while (pos < s.size() && s[pos] != ',' && s[pos] != '}' &&
           s[pos] != ' ' && s[pos] != '\t' && s[pos] != '\r' && s[pos] != '\n')
    {
        out += s[pos];
        pos++;
    }
    return out;
}

std::string json_escape(const std::string &value)
{
    std::string out;
    out.reserve(value.size());

    for (char c : value)
    {
        switch (c)
        {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default: out += c; break;
        }
    }

    return out;
}

} // namespace

std::map<std::string, std::string> json_parse_object(const std::string &json)
{
    std::map<std::string, std::string> result;

    size_t pos = 0;
    skip_whitespace(json, pos);

    if (pos >= json.size() || json[pos] != '{')
    {
        return result; // not an object - empty map
    }
    pos++; // skip '{'

    skip_whitespace(json, pos);
    if (pos < json.size() && json[pos] == '}')
    {
        return result; // empty object "{}"
    }

    while (pos < json.size())
    {
        skip_whitespace(json, pos);

        std::string key;
        if (!parse_string_literal(json, pos, key))
        {
            return {}; // malformed - bail with an empty map rather than a partial one
        }

        skip_whitespace(json, pos);
        if (pos >= json.size() || json[pos] != ':')
        {
            return {};
        }
        pos++; // skip ':'
        skip_whitespace(json, pos);

        std::string value;
        if (pos < json.size() && json[pos] == '"')
        {
            if (!parse_string_literal(json, pos, value))
            {
                return {};
            }
        }
        else
        {
            value = parse_bare_literal(json, pos);
        }

        result[key] = value;

        skip_whitespace(json, pos);
        if (pos < json.size() && json[pos] == ',')
        {
            pos++;
            continue;
        }
        else if (pos < json.size() && json[pos] == '}')
        {
            break;
        }
        else
        {
            return {}; // malformed - missing ',' or '}'
        }
    }

    return result;
}

std::string json_build_object(const std::vector<std::pair<std::string, std::string>> &fields)
{
    std::string out = "{";

    for (size_t i = 0; i < fields.size(); i++)
    {
        if (i > 0)
        {
            out += ",";
        }
        out += "\"" + fields[i].first + "\":\"" + json_escape(fields[i].second) + "\"";
    }

    out += "}";
    return out;
}

std::string json_build_array_of_objects(const std::vector<std::vector<std::pair<std::string, std::string>>> &objects)
{
    std::string out = "[";

    for (size_t i = 0; i < objects.size(); i++)
    {
        if (i > 0)
        {
            out += ",";
        }
        out += json_build_object(objects[i]);
    }

    out += "]";
    return out;
}
