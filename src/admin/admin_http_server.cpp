#include "admin_http_server.h"

#include <pthread.h>
#include <cstring>
#include <cerrno>
#include <cctype>
#include <cstdlib>
#include <atomic>
#include <map>
#include <vector>
#include <utility>
#include <fstream>
#include <sstream>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

#include <openssl/crypto.h>

#include "admin_session.h"
#include "auth_failure_log.h"
#include "log.h"

static const char *TAG = "ADMIN_HTTP";

// Routes this server handles (fixed if/else dispatch in handle_request()
// below, matching rtsp_server.cpp's dispatch()/main.cpp's
// on_signaling_message() style rather than a router table):
//   GET  /        -> valid admin_session cookie required, else 302 /login;
//                    else serves admin_dashboard.html with
//                    %%SIGNALING_TOKEN%%/%%SIGNALING_PORT%%/%%ADMIN_SESSION_ID%%
//                    substituted in
//   GET  /login   -> serves admin_login.html verbatim
//   POST /login   -> validates ADMIN_USERNAME/ADMIN_PASSWORD (form body),
//                    rate-limited via auth_failure_log.h same as
//                    RTSP/signaling; on success sets the admin_session
//                    cookie and redirects to /, else redirects to
//                    /login?error=1
//   POST /logout  -> destroys the session, clears the cookie, redirects
//                    to /login
//   anything else -> 404

static int g_listen_fd = -1;
static pthread_t g_accept_thread;
static std::atomic<bool> g_running(false);

static std::string g_admin_username;
static std::string g_admin_password;
static std::string g_signaling_token;
static uint16_t g_signaling_port = 0;

struct connection_ctx_t
{
    int fd;
    std::string client_ip;
};

// ----------------------------------------------------------------------
// Small, self-contained HTTP/URL/cookie parsing helpers.
//
// Deliberately NOT shared with signaling_server.cpp's own header
// parser, even though the shapes overlap - that file's parser is
// method-agnostic (only ever needed to find Upgrade/Sec-WebSocket-Key),
// this one needs the method+path+body too, and pulling a shared module
// out of signaling_server.cpp isn't worth touching a file this project
// otherwise has no reason to change for this feature. Same kind of
// small, intentional duplication this codebase already has elsewhere
// (see e.g. log_openssl_errors() in more than one file).
// ----------------------------------------------------------------------

static std::string to_lower(const std::string &s)
{
    std::string out = s;
    for (char &c : out)
    {
        c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    }
    return out;
}

// Parses the request line + headers of `block` (everything up to, but
// not including, the blank-line terminator). Returns false if the
// request line itself is malformed. Header keys are stored lowercased
// for case-insensitive lookup (HTTP header names are case-insensitive
// per RFC 7230).
static bool parse_http_request_head(
    const std::string &block, std::string &out_method, std::string &out_path,
    std::map<std::string, std::string> &out_headers)
{
    size_t line_end = block.find("\r\n");
    if (line_end == std::string::npos)
    {
        return false;
    }

    std::string request_line = block.substr(0, line_end);
    size_t sp1 = request_line.find(' ');
    size_t sp2 = (sp1 == std::string::npos) ? std::string::npos : request_line.find(' ', sp1 + 1);
    if (sp1 == std::string::npos || sp2 == std::string::npos)
    {
        return false;
    }

    out_method = request_line.substr(0, sp1);
    out_path = request_line.substr(sp1 + 1, sp2 - sp1 - 1);

    size_t pos = line_end + 2;
    while (pos < block.size())
    {
        size_t next = block.find("\r\n", pos);
        if (next == std::string::npos)
        {
            break;
        }

        std::string line = block.substr(pos, next - pos);
        pos = next + 2;

        if (line.empty())
        {
            break; // blank line - shouldn't normally appear before the caller's own terminator, but stop cleanly either way
        }

        size_t colon = line.find(':');
        if (colon == std::string::npos)
        {
            continue; // malformed header line - skip rather than fail the whole request
        }

        std::string key = to_lower(line.substr(0, colon));
        size_t value_start = colon + 1;
        while (value_start < line.size() && line[value_start] == ' ')
        {
            value_start++;
        }

        out_headers[key] = line.substr(value_start);
    }

    return true;
}

static int hex_digit_value(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// RFC 3986 %XX decoding + '+' -> space, as used by
// application/x-www-form-urlencoded bodies (the admin login form).
static std::string url_decode(const std::string &s)
{
    std::string out;
    out.reserve(s.size());

    for (size_t i = 0; i < s.size(); i++)
    {
        if (s[i] == '+')
        {
            out += ' ';
        }
        else if (s[i] == '%' && i + 2 < s.size())
        {
            int hi = hex_digit_value(s[i + 1]);
            int lo = hex_digit_value(s[i + 2]);
            if (hi >= 0 && lo >= 0)
            {
                out += static_cast<char>((hi << 4) | lo);
                i += 2;
            }
            else
            {
                out += s[i]; // malformed escape - keep the literal '%'
            }
        }
        else
        {
            out += s[i];
        }
    }

    return out;
}

static std::map<std::string, std::string> parse_urlencoded_body(const std::string &body)
{
    std::map<std::string, std::string> result;

    size_t pos = 0;
    while (pos < body.size())
    {
        size_t amp = body.find('&', pos);
        std::string pair = body.substr(pos, amp == std::string::npos ? std::string::npos : amp - pos);

        size_t eq = pair.find('=');
        if (eq != std::string::npos)
        {
            result[url_decode(pair.substr(0, eq))] = url_decode(pair.substr(eq + 1));
        }

        if (amp == std::string::npos)
        {
            break;
        }
        pos = amp + 1;
    }

    return result;
}

// Extracts one named value from a "Cookie: a=1; b=2; ..." header
// value. Returns "" if absent - callers treat that identically to "no
// cookie sent at all".
static std::string get_cookie_value(const std::string &cookie_header, const std::string &name)
{
    size_t pos = 0;
    while (pos < cookie_header.size())
    {
        size_t semi = cookie_header.find(';', pos);
        std::string pair = cookie_header.substr(pos, semi == std::string::npos ? std::string::npos : semi - pos);

        size_t start = pair.find_first_not_of(' ');
        if (start != std::string::npos)
        {
            pair = pair.substr(start);
        }

        size_t eq = pair.find('=');
        if (eq != std::string::npos && pair.substr(0, eq) == name)
        {
            return pair.substr(eq + 1);
        }

        if (semi == std::string::npos)
        {
            break;
        }
        pos = semi + 1;
    }

    return "";
}

static bool read_file_to_string(const std::string &path, std::string &out)
{
    std::ifstream f(path, std::ios::binary);
    if (!f)
    {
        return false;
    }

    std::ostringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return true;
}

static void replace_all(std::string &s, const std::string &from, const std::string &to)
{
    size_t pos = 0;
    while ((pos = s.find(from, pos)) != std::string::npos)
    {
        s.replace(pos, from.size(), to);
        pos += to.size();
    }
}

static std::string build_http_response(
    int status_code, const std::string &status_text,
    const std::vector<std::pair<std::string, std::string>> &headers,
    const std::string &body)
{
    std::ostringstream resp;
    resp << "HTTP/1.1 " << status_code << " " << status_text << "\r\n";
    resp << "Content-Length: " << body.size() << "\r\n";
    resp << "Connection: close\r\n";

    for (const auto &h : headers)
    {
        resp << h.first << ": " << h.second << "\r\n";
    }

    resp << "\r\n" << body;
    return resp.str();
}

static void send_response(int fd, const std::string &response)
{
    send(fd, response.data(), response.size(), 0);
}

// ----------------------------------------------------------------------
// Route handlers
// ----------------------------------------------------------------------

static void handle_get_root(int fd, const std::map<std::string, std::string> &headers)
{
    auto cookie_it = headers.find("cookie");
    std::string cookie_header = (cookie_it != headers.end()) ? cookie_it->second : "";
    std::string session_id = get_cookie_value(cookie_header, "admin_session");

    if (!admin_session_is_valid(session_id))
    {
        send_response(fd, build_http_response(302, "Found", {{"Location", "/login"}}, ""));
        return;
    }

    std::string html;
    if (!read_file_to_string("admin_dashboard.html", html))
    {
        LOG_ERROR(TAG, "admin_dashboard.html not found (expected in the process's CWD)");
        send_response(fd, build_http_response(500, "Internal Server Error", {}, "admin_dashboard.html missing"));
        return;
    }

    // Injected server-side so the real SIGNALING_TOKEN never has to be
    // typed/pasted by a human, and only ever reaches a browser that
    // already proved a valid admin login - see admin_http_server.h.
    replace_all(html, "%%SIGNALING_TOKEN%%", g_signaling_token);
    replace_all(html, "%%SIGNALING_PORT%%", std::to_string(g_signaling_port));
    replace_all(html, "%%ADMIN_SESSION_ID%%", session_id);

    send_response(fd, build_http_response(200, "OK", {{"Content-Type", "text/html; charset=utf-8"}}, html));
}

static void handle_get_login(int fd)
{
    std::string html;
    if (!read_file_to_string("admin_login.html", html))
    {
        LOG_ERROR(TAG, "admin_login.html not found (expected in the process's CWD)");
        send_response(fd, build_http_response(500, "Internal Server Error", {}, "admin_login.html missing"));
        return;
    }

    send_response(fd, build_http_response(200, "OK", {{"Content-Type", "text/html; charset=utf-8"}}, html));
}

static void handle_post_login(int fd, const std::string &body, const std::string &client_ip)
{
    if (auth_failure_is_blocked("ADMIN", client_ip))
    {
        send_response(fd, build_http_response(429, "Too Many Requests", {}, ""));
        return;
    }

    std::map<std::string, std::string> form = parse_urlencoded_body(body);
    std::string username = form.count("username") ? form["username"] : "";
    std::string password = form.count("password") ? form["password"] : "";
    bool remember_me = form.count("remember") && !form["remember"].empty();

    // Same constant-time-compare pattern as signaling_server.cpp's
    // SIGNALING_TOKEN check and rtsp_server.cpp's Digest auth: compare
    // lengths first (CRYPTO_memcmp itself doesn't validate that), then
    // CRYPTO_memcmp so a failed match doesn't leak timing information
    // about how many leading bytes were correct.
    bool username_ok = username.size() == g_admin_username.size() &&
                        CRYPTO_memcmp(username.data(), g_admin_username.data(), username.size()) == 0;
    bool password_ok = password.size() == g_admin_password.size() &&
                        CRYPTO_memcmp(password.data(), g_admin_password.data(), password.size()) == 0;

    if (!username_ok || !password_ok)
    {
        auth_failure_log("ADMIN", client_ip, "bad username or password");
        send_response(fd, build_http_response(302, "Found", {{"Location", "/login?error=1"}}, ""));
        return;
    }

    std::string session_id = admin_session_create(username, remember_me);
    if (session_id.empty())
    {
        // admin_session_create() already logged why (RAND_bytes failure).
        send_response(fd, build_http_response(500, "Internal Server Error", {}, ""));
        return;
    }

    long max_age_seconds = remember_me ? (30L * 24 * 60 * 60) : (24L * 60 * 60);
    std::string cookie = "admin_session=" + session_id +
                          "; HttpOnly; Path=/; SameSite=Strict; Max-Age=" + std::to_string(max_age_seconds);

    LOG_INFO(TAG, "admin login OK for user=%s from %s (remember_me=%d)",
             username.c_str(), client_ip.c_str(), remember_me ? 1 : 0);

    send_response(fd, build_http_response(302, "Found", {{"Location", "/"}, {"Set-Cookie", cookie}}, ""));
}

static void handle_post_logout(int fd, const std::map<std::string, std::string> &headers)
{
    auto cookie_it = headers.find("cookie");
    std::string cookie_header = (cookie_it != headers.end()) ? cookie_it->second : "";
    std::string session_id = get_cookie_value(cookie_header, "admin_session");

    if (!session_id.empty())
    {
        admin_session_destroy(session_id);
    }

    std::string cleared_cookie = "admin_session=; HttpOnly; Path=/; SameSite=Strict; Max-Age=0";
    send_response(fd, build_http_response(302, "Found", {{"Location", "/login"}, {"Set-Cookie", cleared_cookie}}, ""));
}

static void handle_request(
    int fd, const std::string &method, const std::string &path,
    const std::map<std::string, std::string> &headers,
    const std::string &body, const std::string &client_ip)
{
    if (method == "GET" && path == "/")
    {
        handle_get_root(fd, headers);
    }
    else if (method == "GET" && path == "/login")
    {
        handle_get_login(fd);
    }
    else if (method == "POST" && path == "/login")
    {
        handle_post_login(fd, body, client_ip);
    }
    else if (method == "POST" && path == "/logout")
    {
        handle_post_logout(fd, headers);
    }
    else
    {
        send_response(fd, build_http_response(404, "Not Found", {}, ""));
    }
}

// ----------------------------------------------------------------------
// Connection handling (one request per connection - Connection: close
// is always sent, no keep-alive/pipelining support needed for a login
// form + occasional dashboard load, same "simplest thing that works
// for this traffic pattern" reasoning as signaling_server.cpp's
// WebSocket-only design).
// ----------------------------------------------------------------------

static void *connection_thread_func(void *arg)
{
    connection_ctx_t *ctx = static_cast<connection_ctx_t *>(arg);
    int fd = ctx->fd;
    std::string client_ip = ctx->client_ip;
    delete ctx;

    std::string buffer;
    char chunk[4096];
    size_t header_end;

    while ((header_end = buffer.find("\r\n\r\n")) == std::string::npos)
    {
        ssize_t n = recv(fd, chunk, sizeof(chunk), 0);
        if (n <= 0)
        {
            close(fd);
            return nullptr;
        }
        buffer.append(chunk, static_cast<size_t>(n));

        if (buffer.size() > 65536)
        {
            LOG_WARN(TAG, "request head from %s too large, rejecting", client_ip.c_str());
            close(fd);
            return nullptr;
        }
    }

    std::string method, path;
    std::map<std::string, std::string> headers;
    // header_end points at the START of the blank-line "\r\n\r\n"
    // terminator, which overlaps with the last header line's own
    // trailing "\r\n" - +2 includes that terminator so the parser's
    // find("\r\n", ...) loop can actually find the end of the LAST
    // header line, not just every header before it.
    if (!parse_http_request_head(buffer.substr(0, header_end + 2), method, path, headers))
    {
        LOG_WARN(TAG, "malformed HTTP request from %s, closing connection", client_ip.c_str());
        close(fd);
        return nullptr;
    }

    std::string body = buffer.substr(header_end + 4);

    size_t content_length = 0;
    auto cl_it = headers.find("content-length");
    if (cl_it != headers.end())
    {
        long parsed = std::atol(cl_it->second.c_str());
        content_length = (parsed > 0) ? static_cast<size_t>(parsed) : 0;
    }

    // The login form's body is tiny (a couple of short fields) -
    // 1MB is generous headroom while still bounding how much an
    // unauthenticated POST can make this thread buffer.
    if (content_length > (1u << 20))
    {
        LOG_WARN(TAG, "request body from %s too large (%zu bytes), rejecting", client_ip.c_str(), content_length);
        close(fd);
        return nullptr;
    }

    while (body.size() < content_length)
    {
        ssize_t n = recv(fd, chunk, sizeof(chunk), 0);
        if (n <= 0)
        {
            break; // connection closed early - handle_request() below just sees a short body
        }
        body.append(chunk, static_cast<size_t>(n));
    }
    if (body.size() > content_length)
    {
        body.resize(content_length);
    }

    handle_request(fd, method, path, headers, body, client_ip);

    close(fd);
    return nullptr;
}

static void *accept_thread_func(void *arg)
{
    (void)arg;

    LOG_INFO(TAG, "accept thread started");

    while (g_running)
    {
        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);

        int client_fd = accept(g_listen_fd, reinterpret_cast<struct sockaddr *>(&client_addr), &addr_len);
        if (client_fd < 0)
        {
            continue;
        }

        if (!g_running)
        {
            close(client_fd);
            break;
        }

        char ip_str[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &client_addr.sin_addr, ip_str, sizeof(ip_str));

        connection_ctx_t *ctx = new connection_ctx_t{client_fd, std::string(ip_str)};

        pthread_t conn_thread;
        if (pthread_create(&conn_thread, nullptr, connection_thread_func, ctx) != 0)
        {
            LOG_WARN(TAG, "failed to spawn connection thread for %s", ip_str);
            delete ctx;
            close(client_fd);
            continue;
        }

        pthread_detach(conn_thread);
    }

    LOG_INFO(TAG, "accept thread exit");
    return nullptr;
}

int admin_http_server_start(
    uint16_t port,
    const std::string &admin_username,
    const std::string &admin_password,
    const std::string &signaling_token,
    uint16_t signaling_port)
{
    g_admin_username = admin_username;
    g_admin_password = admin_password;
    g_signaling_token = signaling_token;
    g_signaling_port = signaling_port;

    g_listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (g_listen_fd < 0)
    {
        LOG_ERROR(TAG, "socket() failed: %s", strerror(errno));
        return -1;
    }

    int reuse = 1;
    setsockopt(g_listen_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    if (bind(g_listen_fd, reinterpret_cast<struct sockaddr *>(&addr), sizeof(addr)) < 0)
    {
        if (errno == EACCES)
        {
            // Ports below 1024 need CAP_NET_BIND_SERVICE - this project
            // doesn't run as root, so the binary needs the capability
            // granted explicitly (once per build/rebuild, since it
            // doesn't survive relinking).
            LOG_ERROR(TAG, "bind() failed on port %u: permission denied. Ports below 1024 need "
                           "the CAP_NET_BIND_SERVICE capability - run "
                           "'sudo setcap cap_net_bind_service=+ep <path to camera_app>' once "
                           "(after every rebuild) rather than running this process as root.", port);
        }
        else
        {
            LOG_ERROR(TAG, "bind() failed on port %u: %s", port, strerror(errno));
        }
        close(g_listen_fd);
        g_listen_fd = -1;
        return -1;
    }

    if (listen(g_listen_fd, 16) < 0)
    {
        LOG_ERROR(TAG, "listen() failed: %s", strerror(errno));
        close(g_listen_fd);
        g_listen_fd = -1;
        return -1;
    }

    g_running = true;

    if (pthread_create(&g_accept_thread, nullptr, accept_thread_func, nullptr) != 0)
    {
        LOG_ERROR(TAG, "pthread_create failed: %s", strerror(errno));
        close(g_listen_fd);
        g_listen_fd = -1;
        g_running = false;
        return -1;
    }

    LOG_INFO(TAG, "listening on port %u", port);

    return 0;
}

void admin_http_server_stop(void)
{
    g_running = false;

    if (g_listen_fd >= 0)
    {
        shutdown(g_listen_fd, SHUT_RDWR);
        close(g_listen_fd);
        g_listen_fd = -1;
    }

    pthread_join(g_accept_thread, nullptr);

    g_admin_username.clear();
    g_admin_password.clear();
    g_signaling_token.clear();
}
