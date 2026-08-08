#include "rtsp_message.h"

#include <sstream>
#include <algorithm>
#include <cctype>

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

bool rtsp_parse_request(const std::string &raw, rtsp_request_t *out)
{
    if (!out)
    {
        return false;
    }

    size_t header_end = raw.find("\r\n\r\n");
    size_t sep_len = 4;
    if (header_end == std::string::npos)
    {
        header_end = raw.find("\n\n");
        sep_len = 2;
        if (header_end == std::string::npos)
        {
            return false;
        }
    }

    std::string header_block = raw.substr(0, header_end);
    out->body = raw.substr(header_end + sep_len);

    std::istringstream stream(header_block);
    std::string line;

    if (!std::getline(stream, line))
    {
        return false;
    }

    line = trim(line);
    std::istringstream request_line(line);
    if (!(request_line >> out->method >> out->uri >> out->version))
    {
        return false;
    }

    while (std::getline(stream, line))
    {
        line = trim(line);
        if (line.empty())
        {
            continue;
        }

        size_t colon = line.find(':');
        if (colon == std::string::npos)
        {
            continue;
        }

        std::string name = trim(line.substr(0, colon));
        std::string value = trim(line.substr(colon + 1));
        out->headers[name] = value;
    }

    return true;
}

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
