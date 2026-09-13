// A small blocking HTTP/1.1 server for the receiver's localhost control
// surface (status, and config/stations admin GET/PUT). A dedicated accept
// thread
// hands each connection off to a small fixed worker pool (kWorkers)
// rather than handling it inline, so a stalled/slow peer occupying a
// worker for its full SO_RCVTIMEO doesn't wedge the accept loop against
// every other client -- a soft-DoS once status.host is reachable from
// more than one client, e.g. a LAN status.host in the split-host
// topology.
//
// Request parsing is delegated to vendored picohttpparser
// (src/third_party/) -- the request line, header block and their edge
// cases are exactly the part not worth hand-rolling. Everything else
// (routing, Basic auth, body read, response framing) is here and small.
//
// Deliberate limits, matching the traffic it serves (a handful of local/LAN
// clients, requests seconds apart -- not a general-purpose web server):
//   * `Connection: close` always, one request per accepted connection
//   * `Content-Length` bodies only -- `Transfer-Encoding: chunked` is
//     refused with 411 (webui's httpx client always sends a length)
//   * header block capped at kMaxHeader, body at kMaxBody
//   * a full accepted-but-unqueued backlog (kMaxQueued, all workers busy)
//     closes the new connection immediately rather than growing the queue
//     without bound
//   * every socket is O_CLOEXEC: --watch reloads via execv() without
//     unwinding, so no fd (listener, a queued connection, or one a worker
//     is mid-`handle()` on) must leak into the new image
#pragma once

#include <atomic>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <strings.h> // strncasecmp
#include <sys/socket.h>
#include <unistd.h>

#include "third_party/picohttpparser.h"

namespace fmrx {

// ---- request / response ------------------------------------------------

struct HttpRequest {
    std::string method;
    std::string path;  // request target with any '?query' stripped
    std::string query; // raw, after the first '?' ("" if none)
    std::string body;
    // header names are lower-cased; values as received (trimmed)
    std::vector<std::pair<std::string, std::string>> headers;

    const std::string* header(const char* lower_name) const {
        for (const auto& h : headers)
            if (h.first == lower_name)
                return &h.second;
        return nullptr;
    }
};

struct HttpResponse {
    int status = 200;
    std::string content_type = "application/json";
    std::string body;
    std::vector<std::pair<std::string, std::string>> extra_headers;

    static HttpResponse json(int status, std::string body) {
        return {status, "application/json", std::move(body), {}};
    }
    static HttpResponse text(int status, std::string body) {
        return {status, "text/plain; charset=utf-8", std::move(body), {}};
    }
    // {"error": "..."} with the message JSON-escaped.
    static HttpResponse error(int status, const std::string& msg);
};

using HttpHandler = std::function<HttpResponse(const HttpRequest&)>;

struct HttpRoute {
    std::string method;
    std::string path;
    HttpHandler handler;
};

// ---- small helpers (auth, escaping) ----------------------------------

namespace http_detail {

inline std::string json_escape(const std::string& s) {
    std::string o;
    o.reserve(s.size() + 2);
    for (char c : s) {
        switch (c) {
        case '"': o += "\\\""; break;
        case '\\': o += "\\\\"; break;
        case '\n': o += "\\n"; break;
        case '\r': o += "\\r"; break;
        case '\t': o += "\\t"; break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {
                char b[8];
                std::snprintf(b, sizeof(b), "\\u%04x",
                              static_cast<unsigned char>(c));
                o += b;
            } else {
                o += c;
            }
        }
    }
    return o;
}

// json_escape wrapped in double quotes -- a complete JSON string literal.
inline std::string json_quote(const std::string& s) {
    return "\"" + json_escape(s) + "\"";
}

// A degenerate DSP metric (division by a near-zero denominator, log of a
// non-positive value, ...) can produce nan/inf; std::snprintf("%f", ...)
// on one of those prints the bare token `nan`/`inf`, which is not valid
// JSON and breaks every parser downstream. Every numeric field in a
// status/check JSON response should route through this instead of
// formatting the double directly. `null` is JSON's own "no value" --
// consumers already treat it that way (e.g. relay.py's audio_rate check).
inline std::string json_num(double v, int precision = 2) {
    if (!std::isfinite(v))
        return "null";
    char b[32];
    std::snprintf(b, sizeof(b), "%.*f", precision, v);
    return b;
}

// Standard base64 decode. Returns "" on any malformed input (wrong
// padding, stray characters) -- callers treat that as "no credentials".
inline std::string b64_decode(const std::string& in) {
    auto val = [](unsigned char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;
    };
    if (in.size() % 4 != 0)
        return {};
    std::string out;
    out.reserve(in.size() / 4 * 3);
    for (size_t i = 0; i < in.size(); i += 4) {
        int a = val(in[i]), b = val(in[i + 1]);
        int c = in[i + 2] == '=' ? -2 : val(in[i + 2]);
        int d = in[i + 3] == '=' ? -2 : val(in[i + 3]);
        if (a < 0 || b < 0 || c == -1 || d == -1)
            return {};
        if (c == -2 && (d != -2 || i + 4 != in.size()))
            return {}; // '=' only valid as the last one or two chars
        if (d == -2 && i + 4 != in.size())
            return {};
        out.push_back(static_cast<char>((a << 2) | (b >> 4)));
        if (c != -2)
            out.push_back(static_cast<char>((b << 4) | (c >> 2)));
        if (d != -2)
            out.push_back(static_cast<char>((c << 6) | d));
    }
    return out;
}

// Length-independent-ish equality: always compares max(len) bytes so a
// wrong-length guess isn't distinguishable from a wrong-content one by
// timing. Good enough for a shared local admin password.
inline bool ct_equal(const std::string& a, const std::string& b) {
    const size_t n = a.size() > b.size() ? a.size() : b.size();
    unsigned diff = a.size() ^ b.size();
    for (size_t i = 0; i < n; ++i)
        diff |= static_cast<unsigned char>(i < a.size() ? a[i] : 0) ^
                static_cast<unsigned char>(i < b.size() ? b[i] : 0);
    return diff == 0;
}

} // namespace http_detail

// HTTP Basic: decode the `Authorization` header, compare the password
// portion (everything after the first ':') against `expected` in constant
// time. The username is ignored. Returns false if the
// header is absent or malformed.
inline bool basic_auth_ok(const HttpRequest& req, const std::string& expected) {
    const std::string* h = req.header("authorization");
    if (h == nullptr)
        return false;
    static const char kPrefix[] = "Basic ";
    if (h->size() <= sizeof(kPrefix) - 1 ||
        strncasecmp(h->c_str(), kPrefix, sizeof(kPrefix) - 1) != 0)
        return false;
    const std::string dec =
        http_detail::b64_decode(h->substr(sizeof(kPrefix) - 1));
    const size_t colon = dec.find(':');
    const std::string pass =
        colon == std::string::npos ? std::string() : dec.substr(colon + 1);
    return http_detail::ct_equal(pass, expected);
}

// 401 with the browser-login-prompt header. `realm` shows in the prompt.
inline HttpResponse http_unauthorized(const char* realm = "feedmyfm-rx") {
    HttpResponse r = HttpResponse::error(401, "authentication required");
    r.extra_headers.push_back(
        {"WWW-Authenticate", std::string("Basic realm=\"") + realm + "\""});
    return r;
}

inline HttpResponse HttpResponse::error(int status, const std::string& msg) {
    return HttpResponse::json(
        status, "{\"error\":\"" + http_detail::json_escape(msg) + "\"}");
}

// ---- server ----------------------------------------------------------

class HttpServer {
public:
    // kMaxHeader: the request line + all headers. kMaxBody: the largest
    // Content-Length accepted (config.yml / stations.yml are ~2 KB; this
    // is a generous abuse cap, not a real limit).
    static constexpr size_t kMaxHeader = 32 * 1024;
    static constexpr size_t kMaxBody = 1 * 1024 * 1024;

    // kWorkers: enough that one or two stalled peers can't starve every
    // other client, not a general-purpose server's concurrency budget --
    // this control port serves a handful of clients at most.
    // kMaxQueued: accepted-but-not-yet-handled connections a burst can pile
    // up before new ones are refused outright, bounding worst-case memory
    // (each is just one int fd) and, more importantly, worst-case latency
    // for the connection at the back of that queue.
    static constexpr int kWorkers = 4;
    static constexpr size_t kMaxQueued = 32;

    // bind_host: "127.0.0.1" for loopback-only. routes: matched in order,
    // exact method + exact path.
    HttpServer(const std::string& bind_host, int port,
               std::vector<HttpRoute> routes)
        : m_routes(std::move(routes)) {
        m_fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (m_fd < 0) {
            std::fprintf(stderr, "http: socket() failed (%s)\n",
                         std::strerror(errno));
            return;
        }
        int one = 1;
        ::setsockopt(m_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_port = htons(static_cast<uint16_t>(port));
        if (::inet_pton(AF_INET, bind_host.c_str(), &a.sin_addr) != 1) {
            std::fprintf(stderr, "http: bad bind address '%s'\n",
                         bind_host.c_str());
            ::close(m_fd);
            m_fd = -1;
            return;
        }
        if (::bind(m_fd, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0 ||
            ::listen(m_fd, 8) != 0) {
            std::fprintf(stderr, "http: cannot bind %s:%d (%s)\n",
                         bind_host.c_str(), port, std::strerror(errno));
            ::close(m_fd);
            m_fd = -1;
            return;
        }
        std::fprintf(stderr, "http: listening on %s:%d (%d worker thread(s))\n",
                     bind_host.c_str(), port, kWorkers);
        for (int i = 0; i < kWorkers; ++i)
            m_workers.emplace_back([this] { worker_loop(); });
        m_thread = std::thread([this] { accept_loop(); });
    }

    ~HttpServer() {
        m_stop.store(true);
        if (m_fd >= 0) {
            ::shutdown(m_fd, SHUT_RDWR);
            ::close(m_fd);
        }
        if (m_thread.joinable())
            m_thread.join();
        // Wake every worker blocked in the queue's condition_variable; each
        // notices m_stop (already set above) and exits instead of popping a
        // fd. wait()'s predicate is re-checked before it actually blocks,
        // so this is race-free without holding m_queue_mtx here too.
        m_queue_cv.notify_all();
        for (auto& w : m_workers)
            if (w.joinable())
                w.join();
        // Anything left queued when workers stopped (only possible if
        // construction failed before any workers ever ran) never got a
        // response -- just close the fds so they don't leak.
        for (int c : m_queue)
            ::close(c);
    }

    HttpServer(const HttpServer&) = delete;
    HttpServer& operator=(const HttpServer&) = delete;

    bool ok() const { return m_fd >= 0; }

private:
    static const char* reason(int status) {
        switch (status) {
        case 200: return "OK";
        case 400: return "Bad Request";
        case 401: return "Unauthorized";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 411: return "Length Required";
        case 413: return "Payload Too Large";
        case 422: return "Unprocessable Entity";
        case 431: return "Request Header Fields Too Large";
        case 500: return "Internal Server Error";
        case 503: return "Service Unavailable";
        default: return "OK";
        }
    }

    void send_response(int c, const HttpResponse& r) {
        std::string head = "HTTP/1.1 " + std::to_string(r.status) + " " +
                           reason(r.status) + "\r\n";
        head += "Content-Type: " + r.content_type + "\r\n";
        head += "Content-Length: " + std::to_string(r.body.size()) + "\r\n";
        head += "Connection: close\r\n";
        for (const auto& h : r.extra_headers)
            head += h.first + ": " + h.second + "\r\n";
        head += "\r\n";
        (void)::send(c, head.data(), head.size(), MSG_NOSIGNAL);
        if (!r.body.empty())
            (void)::send(c, r.body.data(), r.body.size(), MSG_NOSIGNAL);
    }

    // Match method + path against the route table. Distinguishes 405
    // (path known, wrong method) from 404.
    HttpResponse dispatch(const HttpRequest& req) {
        bool path_seen = false;
        std::string allow;
        for (const auto& rt : m_routes) {
            if (rt.path != req.path)
                continue;
            path_seen = true;
            if (rt.method == req.method) {
                try {
                    return rt.handler(req);
                } catch (const std::exception& e) {
                    return HttpResponse::error(500, e.what());
                } catch (...) {
                    return HttpResponse::error(500, "unhandled exception");
                }
            }
            allow += (allow.empty() ? "" : ", ") + rt.method;
        }
        if (path_seen) {
            HttpResponse r = HttpResponse::error(405, "method not allowed");
            r.extra_headers.push_back({"Allow", allow});
            return r;
        }
        return HttpResponse::error(404, "not found");
    }

    // Read one request off `c`, parse it, dispatch, respond. Best-effort:
    // on a protocol error we send the matching status and close; on a
    // dead/slow peer we just close.
    void handle(int c) {
        timeval tv{1, 0};
        ::setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        std::string buf;
        const char* method;
        const char* path;
        size_t method_len = 0, path_len = 0;
        int minor_version = 0;
        struct phr_header phr_headers[64];
        size_t prev_len = 0;
        int pret = -2;
        size_t num_headers = sizeof(phr_headers) / sizeof(phr_headers[0]);

        // ---- header block ----
        for (;;) {
            char tmp[8192];
            ssize_t n = ::recv(c, tmp, sizeof(tmp), 0);
            if (n <= 0)
                return; // peer closed or timed out mid-headers
            buf.append(tmp, static_cast<size_t>(n));

            num_headers = sizeof(phr_headers) / sizeof(phr_headers[0]);
            pret = phr_parse_request(buf.data(), buf.size(), &method,
                                     &method_len, &path, &path_len,
                                     &minor_version, phr_headers, &num_headers,
                                     prev_len);
            if (pret > 0)
                break; // full header block parsed; pret = its byte length
            if (pret == -1) {
                send_response(c, HttpResponse::error(400, "malformed request"));
                return;
            }
            // pret == -2: need more. Guard the header size.
            if (buf.size() >= kMaxHeader) {
                send_response(c, HttpResponse::error(
                                     431, "request header fields too large"));
                return;
            }
            prev_len = buf.size();
        }

        HttpRequest req;
        req.method.assign(method, method_len);
        {
            std::string target(path, path_len);
            const size_t q = target.find('?');
            if (q == std::string::npos) {
                req.path = std::move(target);
            } else {
                req.path = target.substr(0, q);
                req.query = target.substr(q + 1);
            }
        }

        size_t content_length = 0;
        bool has_length = false;
        {
            // `num_headers` already holds the final header count from the
            // successful parse that broke the loop above -- no need to
            // parse the same buffer a second time.
            for (size_t i = 0; i < num_headers; ++i) {
                std::string name(phr_headers[i].name, phr_headers[i].name_len);
                for (char& ch : name)
                    ch = static_cast<char>(std::tolower(
                        static_cast<unsigned char>(ch)));
                std::string value(phr_headers[i].value,
                                  phr_headers[i].value_len);
                if (name == "transfer-encoding") {
                    send_response(c, HttpResponse::error(
                                         411, "chunked transfer-encoding not "
                                              "supported; send Content-Length"));
                    return;
                }
                if (name == "content-length") {
                    has_length = true;
                    content_length = 0;
                    for (char d : value) {
                        if (d < '0' || d > '9') {
                            send_response(c, HttpResponse::error(
                                                 400, "bad Content-Length"));
                            return;
                        }
                        content_length = content_length * 10 +
                                         static_cast<size_t>(d - '0');
                        if (content_length > kMaxBody) {
                            send_response(c, HttpResponse::error(
                                                 413, "body too large"));
                            return;
                        }
                    }
                }
                req.headers.emplace_back(std::move(name), std::move(value));
            }
        }

        // ---- body ----
        const size_t header_bytes = static_cast<size_t>(pret);
        size_t have_body = buf.size() - header_bytes;
        if (has_length) {
            while (have_body < content_length) {
                char tmp[8192];
                ssize_t n = ::recv(c, tmp, sizeof(tmp), 0);
                if (n <= 0)
                    return; // truncated body -> just drop the connection
                buf.append(tmp, static_cast<size_t>(n));
                have_body = buf.size() - header_bytes;
            }
            req.body = buf.substr(header_bytes, content_length);
        }

        send_response(c, dispatch(req));
    }

    // Only ever accepts and queues -- never blocks on a peer, so it keeps
    // accepting (and the queue keeps draining via kWorkers) even while
    // every worker is currently stuck on a slow/stalled connection.
    void accept_loop() {
        while (!m_stop.load()) {
            pollfd pfd{m_fd, POLLIN, 0};
            if (::poll(&pfd, 1, 500) <= 0)
                continue;
            int c = ::accept4(m_fd, nullptr, nullptr, SOCK_CLOEXEC);
            if (c < 0)
                continue;
            std::lock_guard<std::mutex> lk(m_queue_mtx);
            if (m_queue.size() >= kMaxQueued) {
                // Every worker is backed up kMaxQueued deep -- refuse
                // rather than let the queue (and this peer's wait) grow
                // without bound.
                ::close(c);
                continue;
            }
            m_queue.push_back(c);
            m_queue_cv.notify_one();
        }
    }

    void worker_loop() {
        for (;;) {
            int c;
            {
                std::unique_lock<std::mutex> lk(m_queue_mtx);
                m_queue_cv.wait(lk,
                                [this] { return m_stop.load() || !m_queue.empty(); });
                if (m_queue.empty()) // woken by m_stop with nothing queued
                    return;
                c = m_queue.front();
                m_queue.pop_front();
            }
            handle(c);
            ::close(c);
        }
    }

    std::vector<HttpRoute> m_routes;
    int m_fd = -1;
    std::atomic<bool> m_stop{false};
    std::thread m_thread; // accept_loop
    std::vector<std::thread> m_workers;
    std::deque<int> m_queue;
    std::mutex m_queue_mtx;
    std::condition_variable m_queue_cv;
};

} // namespace fmrx
