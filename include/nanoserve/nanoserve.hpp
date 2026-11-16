// nanoserve — a tiny, dependency-free HTTP/1.1 + WebSocket (RFC 6455) server
// for Linux, built on epoll. Single header, C++17, MIT licensed.
//
// Threading model: one event-loop thread. All handlers run on the thread that
// calls run()/listen(). Only Server::stop() may be called from another thread
// or from a signal handler.
#pragma once

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace nanoserve {

// ───────────────────────────── utilities ─────────────────────────────────────
namespace detail {

inline uint32_t rotl(uint32_t v, int n) { return (v << n) | (v >> (32 - n)); }

inline std::array<uint8_t, 20> sha1(std::string_view s) {
    uint32_t h0 = 0x67452301, h1 = 0xEFCDAB89, h2 = 0x98BADCFE, h3 = 0x10325476, h4 = 0xC3D2E1F0;
    std::string msg(s);
    msg.push_back(static_cast<char>(0x80));
    while (msg.size() % 64 != 56) msg.push_back('\0');
    const uint64_t bits = static_cast<uint64_t>(s.size()) * 8ull;
    for (int i = 7; i >= 0; --i) msg.push_back(static_cast<char>((bits >> (i * 8)) & 0xFF));

    for (size_t off = 0; off < msg.size(); off += 64) {
        uint32_t w[80];
        for (int i = 0; i < 16; ++i) {
            const auto* b = reinterpret_cast<const uint8_t*>(msg.data()) + off + i * 4;
            w[i] = (uint32_t(b[0]) << 24) | (uint32_t(b[1]) << 16) | (uint32_t(b[2]) << 8) | uint32_t(b[3]);
        }
        for (int i = 16; i < 80; ++i) w[i] = rotl(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        uint32_t a = h0, b = h1, c = h2, d = h3, e = h4;
        for (int i = 0; i < 80; ++i) {
            uint32_t f, k;
            if (i < 20)      { f = (b & c) | (~b & d);          k = 0x5A827999; }
            else if (i < 40) { f = b ^ c ^ d;                   k = 0x6ED9EBA1; }
            else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
            else             { f = b ^ c ^ d;                   k = 0xCA62C1D6; }
            uint32_t t = rotl(a, 5) + f + e + k + w[i];
            e = d; d = c; c = rotl(b, 30); b = a; a = t;
        }
        h0 += a; h1 += b; h2 += c; h3 += d; h4 += e;
    }
    std::array<uint8_t, 20> out{};
    const uint32_t hs[5] = {h0, h1, h2, h3, h4};
    for (int i = 0; i < 5; ++i)
        for (int j = 0; j < 4; ++j) out[i * 4 + j] = static_cast<uint8_t>(hs[i] >> (24 - 8 * j));
    return out;
}

inline std::string base64(const uint8_t* d, size_t n) {
    static const char T[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    size_t i = 0;
    for (; i + 2 < n; i += 3) {
        uint32_t v = (uint32_t(d[i]) << 16) | (uint32_t(d[i + 1]) << 8) | d[i + 2];
        out += T[(v >> 18) & 63]; out += T[(v >> 12) & 63]; out += T[(v >> 6) & 63]; out += T[v & 63];
    }
    if (n - i == 1) {
        uint32_t v = uint32_t(d[i]) << 16;
        out += T[(v >> 18) & 63]; out += T[(v >> 12) & 63]; out += "==";
    } else if (n - i == 2) {
        uint32_t v = (uint32_t(d[i]) << 16) | (uint32_t(d[i + 1]) << 8);
        out += T[(v >> 18) & 63]; out += T[(v >> 12) & 63]; out += T[(v >> 6) & 63]; out += '=';
    }
    return out;
}

inline std::string ws_accept_key(std::string_view client_key) {
    std::string s(client_key);
    s += "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    auto h = sha1(s);
    return base64(h.data(), h.size());
}

inline std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

inline std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.remove_suffix(1);
    return s;
}

inline const char* reason(int code) {
    switch (code) {
        case 101: return "Switching Protocols";
        case 200: return "OK";
        case 201: return "Created";
        case 204: return "No Content";
        case 301: return "Moved Permanently";
        case 302: return "Found";
        case 304: return "Not Modified";
        case 400: return "Bad Request";
        case 401: return "Unauthorized";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 413: return "Payload Too Large";
        case 429: return "Too Many Requests";
        case 431: return "Request Header Fields Too Large";
        case 500: return "Internal Server Error";
        case 501: return "Not Implemented";
        default:  return "Unknown";
    }
}

inline std::vector<std::string> split_path(std::string_view p) {
    std::vector<std::string> out;
    size_t i = 0;
    while (i <= p.size()) {
        size_t j = p.find('/', i);
        if (j == std::string_view::npos) j = p.size();
        out.emplace_back(p.substr(i, j - i));
        i = j + 1;
    }
    return out;
}

inline bool valid_close_code(unsigned c) {
    return (c >= 1000 && c <= 1003) || (c >= 1007 && c <= 1014) || (c >= 3000 && c <= 4999);
}

}  // namespace detail

// ───────────────────────────── public types ──────────────────────────────────

struct Request {
    std::string method, path, query, version;
    std::unordered_map<std::string, std::string> headers;  // keys are lower-case
    std::unordered_map<std::string, std::string> params;   // from "/users/:id"
    std::string body;

    std::string header(const std::string& lower_name) const {
        auto it = headers.find(lower_name);
        return it == headers.end() ? std::string() : it->second;
    }
};

struct Response {
    int status = 200;
    std::vector<std::pair<std::string, std::string>> headers;
    std::string body;

    Response& set(std::string k, std::string v) { headers.emplace_back(std::move(k), std::move(v)); return *this; }
    Response& text(std::string b, int st = 200) { status = st; body = std::move(b); return set("Content-Type", "text/plain; charset=utf-8"); }
    Response& html(std::string b, int st = 200) { status = st; body = std::move(b); return set("Content-Type", "text/html; charset=utf-8"); }
    Response& json(std::string b, int st = 200) { status = st; body = std::move(b); return set("Content-Type", "application/json"); }
};

struct Options {
    size_t max_header_bytes = 16 * 1024;
    size_t max_body_bytes = 1 << 20;
    size_t max_ws_message = 1 << 20;
    size_t max_write_buffer = 8 << 20;   // slow consumers beyond this are dropped
    size_t max_connections = 10000;
    std::chrono::seconds http_idle_timeout{15};
    std::chrono::seconds ws_idle_timeout{90};
    std::chrono::seconds ws_ping_interval{30};
};

class Server;

// Lightweight handle passed to WebSocket callbacks. Copy the id() if you need to
// reach the connection later via Server::send_text(id, ...).
class WsConn {
public:
    uint64_t id() const { return id_; }
    const std::string& path() const { return path_; }
    inline void send_text(std::string_view msg);
    inline void send_binary(std::string_view msg);
    inline void close(uint16_t code = 1000, std::string_view reason = "");

private:
    friend class Server;
    WsConn(Server& s, uint64_t id, const std::string& path) : s_(&s), id_(id), path_(path) {}
    Server* s_;
    uint64_t id_;
    const std::string& path_;
};

struct WsHandlers {
    std::function<void(WsConn&)> on_open;
    std::function<void(WsConn&, std::string_view data, bool is_text)> on_message;
    std::function<void(WsConn&)> on_close;
};

// ───────────────────────────── server ────────────────────────────────────────

class Server {
public:
    using Handler = std::function<void(const Request&, Response&)>;

    explicit Server(Options opt = {}) : opt_(opt) {}
    ~Server() {
        for (auto& kv : conns_) ::close(kv.second->fd);
        if (listen_fd_ >= 0) ::close(listen_fd_);
        if (wake_fd_ >= 0) ::close(wake_fd_);
        if (epfd_ >= 0) ::close(epfd_);
    }
    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    // routing ------------------------------------------------------------------
    Server& route(std::string method, const std::string& pattern, Handler h) {
        routes_.push_back({std::move(method), detail::split_path(pattern), std::move(h)});
        return *this;
    }
    Server& get(const std::string& p, Handler h)  { return route("GET", p, std::move(h)); }
    Server& post(const std::string& p, Handler h) { return route("POST", p, std::move(h)); }
    Server& put(const std::string& p, Handler h)  { return route("PUT", p, std::move(h)); }
    Server& del(const std::string& p, Handler h)  { return route("DELETE", p, std::move(h)); }
    Server& ws(const std::string& path, WsHandlers h) { ws_routes_[path] = std::move(h); return *this; }

    // lifecycle ----------------------------------------------------------------
    // Binds and starts listening. Pass port 0 for an ephemeral port. Returns the actual port.
    uint16_t bind(const std::string& host, uint16_t port) {
        listen_fd_ = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        if (listen_fd_ < 0) throw std::runtime_error("socket() failed");
        int one = 1;
        ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_port = htons(port);
        if (::inet_pton(AF_INET, host.c_str(), &a.sin_addr) != 1) throw std::runtime_error("invalid IPv4 address: " + host);
        if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&a), sizeof a) < 0)
            throw std::runtime_error(std::string("bind() failed: ") + std::strerror(errno));
        if (::listen(listen_fd_, SOMAXCONN) < 0) throw std::runtime_error("listen() failed");
        socklen_t len = sizeof a;
        ::getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&a), &len);
        port_ = ntohs(a.sin_port);

        epfd_ = ::epoll_create1(EPOLL_CLOEXEC);
        wake_fd_ = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
        if (epfd_ < 0 || wake_fd_ < 0) throw std::runtime_error("epoll/eventfd setup failed");
        epoll_add(listen_fd_, EPOLLIN, kListenId);
        epoll_add(wake_fd_, EPOLLIN, kWakeId);
        return port_;
    }

    void run() {
        if (listen_fd_ < 0) throw std::logic_error("call bind() before run()");
        running_ = true;
        epoll_event evs[256];
        auto last_sweep = Clock::now();
        while (running_) {
            int n = ::epoll_wait(epfd_, evs, 256, 1000);
            if (n < 0 && errno != EINTR) break;
            for (int i = 0; i < n; ++i) {
                const uint64_t id = evs[i].data.u64;
                if (id == kListenId) { accept_all(); continue; }
                if (id == kWakeId) { uint64_t v; (void)!::read(wake_fd_, &v, sizeof v); continue; }
                auto it = conns_.find(id);
                if (it == conns_.end() || it->second->closing) continue;
                Conn& c = *it->second;
                if (evs[i].events & (EPOLLERR | EPOLLHUP)) { kill(c); continue; }
                if (evs[i].events & EPOLLOUT) flush(c);
                if (!c.closing && (evs[i].events & EPOLLIN)) on_readable(c);
            }
            reap();
            auto now = Clock::now();
            if (now - last_sweep >= std::chrono::seconds(1)) { last_sweep = now; housekeeping(now); reap(); }
        }
        for (auto& kv : conns_) kill(*kv.second);
        reap();
        running_ = false;
    }

    void listen(const std::string& host, uint16_t port) { bind(host, port); run(); }

    // Async-signal-safe: may be called from another thread or a signal handler.
    void stop() {
        running_ = false;
        if (wake_fd_ >= 0) { uint64_t one = 1; (void)!::write(wake_fd_, &one, sizeof one); }
    }

    uint16_t port() const { return port_; }
    size_t connection_count() const { return conns_.size(); }

    // WebSocket helpers (call from the event-loop thread, e.g. inside handlers) --
    void broadcast(const std::string& path, std::string_view msg, uint64_t exclude_id = 0) {
        const std::string frame = make_frame(0x1, msg);
        for (auto& kv : conns_) {
            Conn& c = *kv.second;
            if (c.state == Conn::Ws && !c.closing && !c.close_after_write && c.id != exclude_id && c.ws_path == path)
                queue_write(c, frame);
        }
    }
    void send_text(uint64_t id, std::string_view msg)   { if (Conn* c = ws_conn(id)) send_frame(*c, 0x1, msg); }
    void send_binary(uint64_t id, std::string_view msg) { if (Conn* c = ws_conn(id)) send_frame(*c, 0x2, msg); }
    void close_ws(uint64_t id, uint16_t code = 1000, std::string_view reason = "") {
        if (Conn* c = ws_conn(id)) send_close(*c, code, reason);
    }

private:
    using Clock = std::chrono::steady_clock;
    static constexpr uint64_t kListenId = 0;
    static constexpr uint64_t kWakeId = UINT64_MAX;

    struct Route { std::string method; std::vector<std::string> segs; Handler h; };

    struct Conn {
        enum State { Http, Ws } state = Http;
        int fd = -1;
        uint64_t id = 0;
        std::string rbuf, wbuf;
        bool closing = false, close_after_write = false, want_out = false, on_close_called = false;
        const WsHandlers* ws = nullptr;
        std::string ws_path, frag;
        uint8_t frag_op = 0;
        bool in_frag = false;
        Clock::time_point last_active = Clock::now(), last_ping = Clock::now();
    };

    Options opt_;
    int listen_fd_ = -1, epfd_ = -1, wake_fd_ = -1;
    uint16_t port_ = 0;
    std::atomic<bool> running_{false};
    uint64_t next_id_ = 1;
    std::vector<Route> routes_;
    std::unordered_map<std::string, WsHandlers> ws_routes_;
    std::unordered_map<uint64_t, std::unique_ptr<Conn>> conns_;
    std::vector<uint64_t> reap_;

    // epoll plumbing -----------------------------------------------------------
    void epoll_add(int fd, uint32_t ev, uint64_t id) {
        epoll_event e{}; e.events = ev; e.data.u64 = id;
        ::epoll_ctl(epfd_, EPOLL_CTL_ADD, fd, &e);
    }
    void set_epollout(Conn& c, bool on) {
        if (c.want_out == on) return;
        c.want_out = on;
        epoll_event e{}; e.events = EPOLLIN | (on ? EPOLLOUT : 0u); e.data.u64 = c.id;
        ::epoll_ctl(epfd_, EPOLL_CTL_MOD, c.fd, &e);
    }
    void accept_all() {
        for (;;) {
            int fd = ::accept4(listen_fd_, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
            if (fd < 0) break;
            if (conns_.size() >= opt_.max_connections) { ::close(fd); continue; }
            int one = 1;
            ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
            auto c = std::make_unique<Conn>();
            c->fd = fd; c->id = next_id_++;
            epoll_add(fd, EPOLLIN, c->id);
            conns_[c->id] = std::move(c);
        }
    }
    void kill(Conn& c) { if (!c.closing) { c.closing = true; reap_.push_back(c.id); } }
    void reap() {
        // on_close handlers may kill other connections, so use an index loop.
        for (size_t i = 0; i < reap_.size(); ++i) {
            auto it = conns_.find(reap_[i]);
            if (it == conns_.end()) continue;
            Conn& c = *it->second;
            if (c.state == Conn::Ws && !c.on_close_called && c.ws && c.ws->on_close) {
                c.on_close_called = true;
                WsConn h(*this, c.id, c.ws_path);
                guarded([&] { c.ws->on_close(h); });
            }
            ::epoll_ctl(epfd_, EPOLL_CTL_DEL, c.fd, nullptr);
            ::close(c.fd);
            conns_.erase(it);
        }
        reap_.clear();
    }
    void housekeeping(Clock::time_point now) {
        for (auto& kv : conns_) {
            Conn& c = *kv.second;
            if (c.closing) continue;
            auto idle = now - c.last_active;
            if (c.state == Conn::Http) {
                if (idle > opt_.http_idle_timeout) kill(c);
            } else {
                if (idle > opt_.ws_idle_timeout) { kill(c); continue; }
                if (now - c.last_ping >= opt_.ws_ping_interval) { c.last_ping = now; send_frame(c, 0x9, ""); }
            }
        }
    }
    template <class F> static void guarded(F&& f) {
        try { f(); }
        catch (const std::exception& e) { std::fprintf(stderr, "[nanoserve] handler threw: %s\n", e.what()); }
        catch (...) { std::fprintf(stderr, "[nanoserve] handler threw unknown exception\n"); }
    }

    // I/O ---------------------------------------------------------------------
    void on_readable(Conn& c) {
        char buf[16384];
        for (int reads = 0; reads < 16 && !c.closing; ++reads) {
            ssize_t n = ::recv(c.fd, buf, sizeof buf, 0);
            if (n > 0) {
                c.last_active = Clock::now();
                if (c.close_after_write) continue;  // drain & discard while closing
                c.rbuf.append(buf, static_cast<size_t>(n));
                process(c);
            } else if (n == 0) { kill(c); return; }
            else if (errno == EINTR) continue;
            else if (errno == EAGAIN || errno == EWOULDBLOCK) return;
            else { kill(c); return; }
        }
    }
    void finish_write(Conn& c) { if (c.close_after_write) kill(c); }
    void queue_write(Conn& c, std::string_view data) {
        if (c.closing) return;
        if (c.wbuf.empty()) {
            size_t off = 0;
            while (off < data.size()) {
                ssize_t n = ::send(c.fd, data.data() + off, data.size() - off, MSG_NOSIGNAL);
                if (n > 0) off += static_cast<size_t>(n);
                else if (n < 0 && errno == EINTR) continue;
                else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
                else { kill(c); return; }
            }
            if (off == data.size()) { finish_write(c); return; }
            c.wbuf.assign(data.substr(off));
        } else {
            c.wbuf.append(data);
        }
        if (c.wbuf.size() > opt_.max_write_buffer) { kill(c); return; }
        set_epollout(c, true);
    }
    void flush(Conn& c) {
        while (!c.wbuf.empty()) {
            ssize_t n = ::send(c.fd, c.wbuf.data(), c.wbuf.size(), MSG_NOSIGNAL);
            if (n > 0) c.wbuf.erase(0, static_cast<size_t>(n));
            else if (n < 0 && errno == EINTR) continue;
            else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return;
            else { kill(c); return; }
        }
        set_epollout(c, false);
        finish_write(c);
    }

    // HTTP --------------------------------------------------------------------
    void process(Conn& c) {
        while (!c.closing && !c.close_after_write && c.state == Conn::Http) {
            size_t pos = c.rbuf.find("\r\n\r\n");
            if (pos == std::string::npos) {
                if (c.rbuf.size() > opt_.max_header_bytes) fail(c, 431);
                return;
            }
            if (pos > opt_.max_header_bytes) { fail(c, 431); return; }
            Request req;
            if (!parse_head(std::string_view(c.rbuf).substr(0, pos), req)) { fail(c, 400); return; }
            const size_t hdr_end = pos + 4;

            if (req.headers.count("transfer-encoding")) { fail(c, 501); return; }
            size_t body_len = 0;
            auto cl = req.headers.find("content-length");
            if (cl != req.headers.end()) {
                const std::string& v = cl->second;
                if (v.empty() || v.size() > 12 ||
                    !std::all_of(v.begin(), v.end(), [](char ch) { return ch >= '0' && ch <= '9'; })) { fail(c, 400); return; }
                body_len = std::strtoull(v.c_str(), nullptr, 10);
                if (body_len > opt_.max_body_bytes) { fail(c, 413); return; }
            }
            if (c.rbuf.size() < hdr_end + body_len) return;  // wait for the rest of the body
            req.body = c.rbuf.substr(hdr_end, body_len);
            c.rbuf.erase(0, hdr_end + body_len);

            if (is_ws_upgrade(req)) { upgrade(c, req); continue; }
            dispatch(c, req);
        }
        if (!c.closing && c.state == Conn::Ws) process_ws(c);
    }

    static bool parse_head(std::string_view head, Request& r) {
        size_t eol = head.find("\r\n");
        std::string_view line = head.substr(0, eol);
        size_t s1 = line.find(' ');
        if (s1 == std::string_view::npos || s1 == 0 || s1 > 15) return false;
        size_t s2 = line.find(' ', s1 + 1);
        if (s2 == std::string_view::npos) return false;
        r.method = std::string(line.substr(0, s1));
        std::string_view target = line.substr(s1 + 1, s2 - s1 - 1);
        r.version = std::string(line.substr(s2 + 1));
        if (r.version.rfind("HTTP/1.", 0) != 0 || target.empty() || target[0] != '/') return false;
        size_t q = target.find('?');
        r.path = std::string(target.substr(0, q));
        if (q != std::string_view::npos) r.query = std::string(target.substr(q + 1));

        size_t pos = (eol == std::string_view::npos) ? head.size() : eol + 2;
        int count = 0;
        while (pos < head.size()) {
            size_t next = head.find("\r\n", pos);
            if (next == std::string_view::npos) next = head.size();
            std::string_view l = head.substr(pos, next - pos);
            size_t colon = l.find(':');
            if (colon == std::string_view::npos || colon == 0 || ++count > 100) return false;
            std::string_view k = l.substr(0, colon);
            if (k.find_first_of(" \t") != std::string_view::npos) return false;
            std::string key = detail::lower(std::string(k));
            std::string val(detail::trim(l.substr(colon + 1)));
            auto it = r.headers.find(key);
            if (it == r.headers.end()) r.headers.emplace(std::move(key), std::move(val));
            else { it->second += ", "; it->second += val; }
            pos = next + 2;
        }
        return true;
    }

    static bool keep_alive(const Request& r) {
        std::string conn = detail::lower(r.header("connection"));
        if (r.version == "HTTP/1.0") return conn.find("keep-alive") != std::string::npos;
        return conn.find("close") == std::string::npos;
    }

    void dispatch(Conn& c, Request& req) {
        Response res;
        bool path_exists = false;
        const Route* hit = nullptr;
        auto segs = detail::split_path(req.path);
        for (const auto& r : routes_) {
            if (r.segs.size() != segs.size()) continue;
            std::unordered_map<std::string, std::string> params;
            bool ok = true;
            for (size_t i = 0; i < segs.size() && ok; ++i) {
                if (!r.segs[i].empty() && r.segs[i][0] == ':') params[r.segs[i].substr(1)] = segs[i];
                else ok = (r.segs[i] == segs[i]);
            }
            if (!ok) continue;
            path_exists = true;
            if (r.method == req.method) { hit = &r; req.params = std::move(params); break; }
        }
        try {
            if (hit) hit->h(req, res);
            else if (path_exists) res.text("Method Not Allowed\n", 405);
            else res.text("Not Found\n", 404);
        } catch (const std::exception& e) {
            std::fprintf(stderr, "[nanoserve] handler threw: %s\n", e.what());
            res = Response{}; res.text("Internal Server Error\n", 500);
        } catch (...) {
            res = Response{}; res.text("Internal Server Error\n", 500);
        }
        send_response(c, res, keep_alive(req));
    }

    void send_response(Conn& c, const Response& res, bool ka) {
        std::string out = "HTTP/1.1 " + std::to_string(res.status) + " " + detail::reason(res.status) + "\r\n";
        out += "Server: nanoserve\r\nContent-Length: " + std::to_string(res.body.size()) + "\r\n";
        out += ka ? "Connection: keep-alive\r\n" : "Connection: close\r\n";
        for (const auto& h : res.headers) {
            // Refuse header injection from handler-provided values.
            if (h.first.find_first_of("\r\n") != std::string::npos || h.second.find_first_of("\r\n") != std::string::npos) continue;
            out += h.first + ": " + h.second + "\r\n";
        }
        out += "\r\n";
        out += res.body;
        if (!ka) c.close_after_write = true;
        queue_write(c, out);
    }

    void fail(Conn& c, int code) {
        Response r; r.text(std::string(detail::reason(code)) + "\n", code);
        c.rbuf.clear();
        send_response(c, r, false);
    }

    // WebSocket ---------------------------------------------------------------
    static bool is_ws_upgrade(const Request& r) {
        return r.method == "GET" &&
               detail::lower(r.header("upgrade")).find("websocket") != std::string::npos &&
               detail::lower(r.header("connection")).find("upgrade") != std::string::npos;
    }

    void upgrade(Conn& c, const Request& req) {
        auto it = ws_routes_.find(req.path);
        std::string key = req.header("sec-websocket-key");
        if (it == ws_routes_.end()) { fail(c, 404); return; }
        if (key.empty() || req.header("sec-websocket-version") != "13") { fail(c, 400); return; }
        std::string resp = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                           "Sec-WebSocket-Accept: " + detail::ws_accept_key(key) + "\r\n\r\n";
        queue_write(c, resp);
        c.state = Conn::Ws;
        c.ws = &it->second;
        c.ws_path = req.path;
        c.last_ping = Clock::now();
        if (c.ws->on_open) { WsConn h(*this, c.id, c.ws_path); guarded([&] { c.ws->on_open(h); }); }
    }

    static std::string make_frame(uint8_t op, std::string_view payload) {
        std::string f;
        f.push_back(static_cast<char>(0x80 | op));
        const size_t n = payload.size();
        if (n < 126) f.push_back(static_cast<char>(n));
        else if (n <= 0xFFFF) { f.push_back(126); f.push_back(static_cast<char>(n >> 8)); f.push_back(static_cast<char>(n & 0xFF)); }
        else { f.push_back(127); for (int i = 7; i >= 0; --i) f.push_back(static_cast<char>((static_cast<uint64_t>(n) >> (i * 8)) & 0xFF)); }
        f.append(payload.data(), payload.size());
        return f;
    }
    void send_frame(Conn& c, uint8_t op, std::string_view payload) {
        if (c.closing || (c.close_after_write && op != 0x8)) return;
        queue_write(c, make_frame(op, payload));
    }
    void send_close(Conn& c, uint16_t code, std::string_view why) {
        if (c.close_after_write || c.closing) return;
        std::string p;
        p.push_back(static_cast<char>(code >> 8)); p.push_back(static_cast<char>(code & 0xFF));
        p.append(why.substr(0, 123));
        c.close_after_write = true;  // set first so finish_write() closes once flushed
        queue_write(c, make_frame(0x8, p));
    }
    Conn* ws_conn(uint64_t id) {
        auto it = conns_.find(id);
        if (it == conns_.end()) return nullptr;
        Conn& c = *it->second;
        return (c.state == Conn::Ws && !c.closing && !c.close_after_write) ? &c : nullptr;
    }

    void deliver(Conn& c, uint8_t op, std::string_view data) {
        if (!c.ws || !c.ws->on_message) return;
        WsConn h(*this, c.id, c.ws_path);
        guarded([&] { c.ws->on_message(h, data, op == 0x1); });
    }

    void process_ws(Conn& c) {
        size_t pos = 0;
        while (!c.closing && !c.close_after_write) {
            const auto* p = reinterpret_cast<const uint8_t*>(c.rbuf.data()) + pos;
            const size_t avail = c.rbuf.size() - pos;
            if (avail < 2) break;
            const bool fin = p[0] & 0x80;
            const uint8_t op = p[0] & 0x0F;
            if (p[0] & 0x70) { send_close(c, 1002, "reserved bits set"); break; }
            if (!(p[1] & 0x80)) { send_close(c, 1002, "client frames must be masked"); break; }
            uint64_t len = p[1] & 0x7F;
            size_t hdr = 2;
            if (len == 126) { if (avail < 4) break; len = (uint64_t(p[2]) << 8) | p[3]; hdr = 4; }
            else if (len == 127) {
                if (avail < 10) break;
                len = 0;
                for (int i = 0; i < 8; ++i) len = (len << 8) | p[2 + i];
                hdr = 10;
            }
            if (op >= 0x8 && (len > 125 || !fin)) { send_close(c, 1002, "bad control frame"); break; }
            if (len > opt_.max_ws_message) { send_close(c, 1009, "message too big"); break; }
            if (avail < hdr + 4 + len) break;
            const uint8_t* mask = p + hdr;
            std::string payload(static_cast<size_t>(len), '\0');
            for (size_t i = 0; i < payload.size(); ++i) payload[i] = static_cast<char>(p[hdr + 4 + i] ^ mask[i & 3]);
            pos += hdr + 4 + static_cast<size_t>(len);

            bool stop = false;
            switch (op) {
                case 0x8: {
                    unsigned code = 1000;
                    if (payload.size() == 1) code = 1002;
                    else if (payload.size() >= 2) {
                        code = (unsigned(uint8_t(payload[0])) << 8) | uint8_t(payload[1]);
                        if (!detail::valid_close_code(code)) code = 1002;
                    }
                    send_close(c, static_cast<uint16_t>(code), "");
                    stop = true;
                    break;
                }
                case 0x9: send_frame(c, 0xA, payload); break;
                case 0xA: break;
                case 0x0:
                    if (!c.in_frag) { send_close(c, 1002, "unexpected continuation"); stop = true; break; }
                    c.frag += payload;
                    if (c.frag.size() > opt_.max_ws_message) { send_close(c, 1009, "message too big"); stop = true; break; }
                    if (fin) {
                        c.in_frag = false;
                        std::string m = std::move(c.frag);
                        c.frag.clear();
                        deliver(c, c.frag_op, m);
                    }
                    break;
                case 0x1: case 0x2:
                    if (c.in_frag) { send_close(c, 1002, "expected continuation"); stop = true; break; }
                    if (fin) deliver(c, op, payload);
                    else { c.in_frag = true; c.frag_op = op; c.frag = std::move(payload); }
                    break;
                default: send_close(c, 1002, "unknown opcode"); stop = true; break;
            }
            if (stop) break;
        }
        c.rbuf.erase(0, pos);
    }
};

inline void WsConn::send_text(std::string_view m)   { s_->send_text(id_, m); }
inline void WsConn::send_binary(std::string_view m) { s_->send_binary(id_, m); }
inline void WsConn::close(uint16_t code, std::string_view r) { s_->close_ws(id_, code, r); }

}  // namespace nanoserve
