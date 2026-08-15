#include "signaling_server.h"

#include <pthread.h>
#include <cstring>
#include <cerrno>
#include <atomic>
#include <map>
#include <vector>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <unistd.h>

#include "sha1.h"
#include "base64.h"
#include "log.h"

static const char *TAG = "SIGNALING_SRV";

// Fixed per RFC 6455 section 1.3 - concatenated onto the client's
// Sec-WebSocket-Key before SHA-1, every WebSocket server uses this
// exact string. Not a secret, just a protocol constant.
static const char *WEBSOCKET_GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

static int g_listen_fd = -1;
static pthread_t g_accept_thread;
static std::atomic<bool> g_running(false);
static signaling_message_handler_t g_handler;

static pthread_mutex_t g_clients_lock = PTHREAD_MUTEX_INITIALIZER;
static std::map<std::string, int> g_clients; // client_id -> fd, for signaling_server_send()

struct connection_ctx_t
{
    int fd;
    std::string client_ip;
};

// ----------------------------------------------------------------------
// WebSocket handshake (RFC 6455 section 4)
// ----------------------------------------------------------------------

// Case-insensitive header lookup - HTTP header names are
// case-insensitive per RFC 7230, and browsers/libraries are free to
// send "Sec-WebSocket-Key" in whatever casing they like.
static bool find_header(const std::map<std::string, std::string> &headers, const std::string &name, std::string &out_value)
{
    std::string lower_name = name;
    for (char &c : lower_name)
    {
        c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    }

    for (const auto &pair : headers)
    {
        std::string lower_key = pair.first;
        for (char &c : lower_key)
        {
            c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
        }

        if (lower_key == lower_name)
        {
            out_value = pair.second;
            return true;
        }
    }

    return false;
}

// Parses the HTTP Upgrade request's header block (everything up to
// and including the blank line - already isolated by the caller) into
// a name->value map. Malformed lines are just skipped rather than
// failing the whole parse - a stray line shouldn't sink a handshake
// that's otherwise fine.
static std::map<std::string, std::string> parse_http_headers(const std::string &block)
{
    std::map<std::string, std::string> headers;

    size_t line_start = block.find("\r\n"); // skip the request line itself (e.g. "GET /signaling HTTP/1.1")
    if (line_start == std::string::npos)
    {
        return headers;
    }
    line_start += 2;

    while (line_start < block.size())
    {
        size_t line_end = block.find("\r\n", line_start);
        if (line_end == std::string::npos)
        {
            break;
        }

        std::string line = block.substr(line_start, line_end - line_start);
        line_start = line_end + 2;

        if (line.empty())
        {
            break; // blank line = end of headers
        }

        size_t colon = line.find(':');
        if (colon == std::string::npos)
        {
            continue;
        }

        std::string key = line.substr(0, colon);
        size_t value_start = colon + 1;
        while (value_start < line.size() && line[value_start] == ' ')
        {
            value_start++;
        }
        std::string value = line.substr(value_start);

        headers[key] = value;
    }

    return headers;
}

// base64(SHA1(client_key + WEBSOCKET_GUID)) - the exact value RFC 6455
// section 4.2.2 step 5 requires for Sec-WebSocket-Accept. This is what
// proves to the client that a WebSocket-aware server (not just any
// HTTP server) handled the Upgrade request.
static std::string compute_ws_accept(const std::string &client_key)
{
    std::string combined = client_key + WEBSOCKET_GUID;
    std::array<uint8_t, 20> digest = sha1(combined);
    std::vector<uint8_t> digest_vec(digest.begin(), digest.end());
    return base64_encode(digest_vec);
}

// Reads the HTTP Upgrade request off `fd`, validates it's a WebSocket
// handshake, and writes back the 101 Switching Protocols response.
// Returns false (and leaves the socket for the caller to close) on
// anything malformed or missing the required headers.
static bool do_websocket_handshake(int fd, const std::string &client_ip)
{
    std::string buffer;
    char chunk[2048];

    // The request line + headers always end in "\r\n\r\n" and (unlike
    // a WebSocket data frame afterwards) is plain HTTP text, so reading
    // in a loop until that terminator shows up is the same technique
    // rtsp_server.cpp's connection_thread_func uses for RTSP requests.
    size_t header_end;
    while ((header_end = buffer.find("\r\n\r\n")) == std::string::npos)
    {
        ssize_t n = recv(fd, chunk, sizeof(chunk), 0);
        if (n <= 0)
        {
            LOG_WARN(TAG, "connection from %s closed before handshake completed", client_ip.c_str());
            return false;
        }
        buffer.append(chunk, static_cast<size_t>(n));

        if (buffer.size() > 8192)
        {
            LOG_WARN(TAG, "handshake request from %s too large, rejecting", client_ip.c_str());
            return false;
        }
    }

    std::map<std::string, std::string> headers = parse_http_headers(buffer);

    std::string upgrade_value;
    std::string ws_key;

    if (!find_header(headers, "Upgrade", upgrade_value) ||
        upgrade_value.find("websocket") == std::string::npos)
    {
        LOG_WARN(TAG, "non-WebSocket request from %s, rejecting", client_ip.c_str());
        return false;
    }

    if (!find_header(headers, "Sec-WebSocket-Key", ws_key) || ws_key.empty())
    {
        LOG_WARN(TAG, "missing Sec-WebSocket-Key from %s, rejecting", client_ip.c_str());
        return false;
    }

    std::string accept_value = compute_ws_accept(ws_key);

    std::string response =
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Accept: " + accept_value + "\r\n"
        "\r\n";

    if (send(fd, response.c_str(), response.size(), 0) < 0)
    {
        LOG_WARN(TAG, "failed to send handshake response to %s: %s", client_ip.c_str(), strerror(errno));
        return false;
    }

    LOG_INFO(TAG, "WebSocket handshake complete with %s", client_ip.c_str());
    return true;
}

// ----------------------------------------------------------------------
// WebSocket framing (RFC 6455 section 5)
// ----------------------------------------------------------------------

enum ws_opcode_t : uint8_t
{
    WS_OPCODE_TEXT = 0x1,
    WS_OPCODE_CLOSE = 0x8,
    WS_OPCODE_PING = 0x9,
    WS_OPCODE_PONG = 0xA,
};

// Builds a single unfragmented, unmasked server->client frame (RFC
// 6455 section 5.2 - servers MUST NOT mask frames they send, only
// clients mask). opcode is WS_OPCODE_TEXT for every message this
// module sends except pong replies (see handle path below).
static std::string build_ws_frame(uint8_t opcode, const std::string &payload)
{
    std::string frame;

    frame += static_cast<char>(0x80 | opcode); // FIN=1, opcode in low 4 bits

    size_t len = payload.size();
    if (len <= 125)
    {
        frame += static_cast<char>(len);
    }
    else if (len <= 0xFFFF)
    {
        frame += static_cast<char>(126);
        frame += static_cast<char>((len >> 8) & 0xFF);
        frame += static_cast<char>(len & 0xFF);
    }
    else
    {
        frame += static_cast<char>(127);
        for (int i = 7; i >= 0; i--)
        {
            frame += static_cast<char>((static_cast<uint64_t>(len) >> (i * 8)) & 0xFF);
        }
    }

    frame += payload;
    return frame;
}

// Attempts to decode ONE complete frame from the front of `buffer`.
// Returns false if `buffer` doesn't yet hold a complete frame (caller
// should recv() more and retry - same "not enough data yet, keep
// reading" pattern as rtsp_server.cpp's find("\r\n\r\n") loop, just
// for binary WebSocket frames instead of text HTTP). On success,
// consumes the frame's bytes from `buffer` and fills opcode/payload.
//
// Client->server frames are always masked (RFC 6455 section 5.1) -
// this function unmasks the payload as part of decoding, since a
// masked payload isn't usable JSON until XORed with its mask key.
static bool decode_ws_frame(std::string &buffer, uint8_t &opcode, std::string &payload)
{
    if (buffer.size() < 2)
    {
        return false;
    }

    uint8_t byte0 = static_cast<uint8_t>(buffer[0]);
    uint8_t byte1 = static_cast<uint8_t>(buffer[1]);

    opcode = byte0 & 0x0F;
    bool masked = (byte1 & 0x80) != 0;
    uint64_t len = byte1 & 0x7F;

    size_t pos = 2;

    if (len == 126)
    {
        if (buffer.size() < pos + 2)
        {
            return false;
        }
        len = (static_cast<uint8_t>(buffer[pos]) << 8) | static_cast<uint8_t>(buffer[pos + 1]);
        pos += 2;
    }
    else if (len == 127)
    {
        if (buffer.size() < pos + 8)
        {
            return false;
        }
        len = 0;
        for (int i = 0; i < 8; i++)
        {
            len = (len << 8) | static_cast<uint8_t>(buffer[pos + i]);
        }
        pos += 8;
    }

    uint8_t mask_key[4] = {0, 0, 0, 0};
    if (masked)
    {
        if (buffer.size() < pos + 4)
        {
            return false;
        }
        for (int i = 0; i < 4; i++)
        {
            mask_key[i] = static_cast<uint8_t>(buffer[pos + i]);
        }
        pos += 4;
    }

    if (buffer.size() < pos + len)
    {
        return false; // full frame not received yet
    }

    payload.assign(buffer, pos, static_cast<size_t>(len));

    if (masked)
    {
        for (size_t i = 0; i < payload.size(); i++)
        {
            payload[i] = static_cast<char>(static_cast<uint8_t>(payload[i]) ^ mask_key[i % 4]);
        }
    }

    buffer.erase(0, pos + static_cast<size_t>(len));
    return true;
}

// ----------------------------------------------------------------------
// Client registry (for signaling_server_send() from other threads)
// ----------------------------------------------------------------------

static void register_client(const std::string &client_id, int fd)
{
    pthread_mutex_lock(&g_clients_lock);
    g_clients[client_id] = fd;
    pthread_mutex_unlock(&g_clients_lock);
}

static void unregister_client(const std::string &client_id)
{
    pthread_mutex_lock(&g_clients_lock);
    g_clients.erase(client_id);
    pthread_mutex_unlock(&g_clients_lock);
}

bool signaling_server_send(const std::string &client_id, const std::string &json)
{
    pthread_mutex_lock(&g_clients_lock);
    auto it = g_clients.find(client_id);
    int fd = (it != g_clients.end()) ? it->second : -1;
    pthread_mutex_unlock(&g_clients_lock);

    if (fd < 0)
    {
        return false;
    }

    std::string frame = build_ws_frame(WS_OPCODE_TEXT, json);
    ssize_t sent = send(fd, frame.c_str(), frame.size(), 0);

    if (sent < 0)
    {
        LOG_WARN(TAG, "failed to send to client %s: %s", client_id.c_str(), strerror(errno));
        return false;
    }

    return true;
}

// ----------------------------------------------------------------------
// Connection handling
// ----------------------------------------------------------------------

static void *connection_thread_func(void *arg)
{
    connection_ctx_t *ctx = static_cast<connection_ctx_t *>(arg);
    int fd = ctx->fd;
    std::string client_ip = ctx->client_ip;
    delete ctx;

    if (!do_websocket_handshake(fd, client_ip))
    {
        close(fd);
        return nullptr;
    }

    // client_id: fd is unique among currently-open connections, which
    // is exactly the lifetime this needs to be unique for - once this
    // connection closes and unregister_client() runs, that same fd
    // value is free to be reused by a brand new (and by then, distinct)
    // client_id, same as e.g. process PIDs being reused after exit.
    std::string client_id = std::to_string(fd);
    register_client(client_id, fd);
    LOG_INFO(TAG, "client %s connected from %s", client_id.c_str(), client_ip.c_str());

    std::string buffer;
    char chunk[4096];

    while (g_running)
    {
        uint8_t opcode;
        std::string payload;

        // Drain every complete frame already buffered before calling
        // recv() again - a single recv() can return more than one
        // frame's worth of bytes (e.g. a burst of ICE candidates sent
        // back-to-back by the browser).
        bool got_frame;
        do
        {
            got_frame = decode_ws_frame(buffer, opcode, payload);
            if (!got_frame)
            {
                break;
            }

            switch (opcode)
            {
                case WS_OPCODE_TEXT:
                    if (g_handler)
                    {
                        g_handler(client_id, payload);
                    }
                    break;

                case WS_OPCODE_PING:
                    // Must reply with the same payload (RFC 6455
                    // section 5.5.2) - browsers/libraries use this to
                    // detect a dead connection, so silently dropping
                    // it would eventually get us disconnected.
                    {
                        std::string pong = build_ws_frame(WS_OPCODE_PONG, payload);
                        send(fd, pong.c_str(), pong.size(), 0);
                    }
                    break;

                case WS_OPCODE_CLOSE:
                    {
                        std::string close_frame = build_ws_frame(WS_OPCODE_CLOSE, "");
                        send(fd, close_frame.c_str(), close_frame.size(), 0);
                    }
                    unregister_client(client_id);
                    close(fd);
                    LOG_INFO(TAG, "client %s closed connection", client_id.c_str());
                    return nullptr;

                default:
                    // WS_OPCODE_PONG and any binary/continuation
                    // opcode: signaling messages are always text, so
                    // there's nothing meaningful to do with these.
                    break;
            }
        } while (got_frame);

        ssize_t n = recv(fd, chunk, sizeof(chunk), 0);
        if (n <= 0)
        {
            break;
        }
        buffer.append(chunk, static_cast<size_t>(n));
    }

    unregister_client(client_id);
    close(fd);
    LOG_INFO(TAG, "client %s disconnected", client_id.c_str());

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

int signaling_server_start(uint16_t port, signaling_message_handler_t handler)
{
    g_handler = handler;

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
        LOG_ERROR(TAG, "bind() failed on port %u: %s", port, strerror(errno));
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
        LOG_ERROR(TAG, "pthread_create (accept) failed: %s", strerror(errno));
        close(g_listen_fd);
        g_listen_fd = -1;
        g_running = false;
        return -1;
    }

    LOG_INFO(TAG, "listening on port %u", port);

    return 0;
}

void signaling_server_stop(void)
{
    g_running = false;

    if (g_listen_fd >= 0)
    {
        shutdown(g_listen_fd, SHUT_RDWR);
        close(g_listen_fd);
        g_listen_fd = -1;
    }

    pthread_join(g_accept_thread, nullptr);

    pthread_mutex_lock(&g_clients_lock);
    g_clients.clear();
    pthread_mutex_unlock(&g_clients_lock);
}
