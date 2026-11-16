#include <nanoserve/nanoserve.hpp>

#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <cstdio>
#include <string>
#include <thread>

using namespace nanoserve;

static int g_fail = 0;
#define CHECK(cond) do { if (!(cond)) { std::fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); ++g_fail; } } while (0)

static int dial(uint16_t port) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    timeval tv{2, 0};
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    sockaddr_in a{}; a.sin_family = AF_INET; a.sin_port = htons(port);
    ::inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0) { std::perror("connect"); std::exit(2); }
    return fd;
}
static void send_all(int fd, const std::string& s) { size_t o = 0; while (o < s.size()) { ssize_t n = ::send(fd, s.data() + o, s.size() - o, MSG_NOSIGNAL); if (n <= 0) break; o += n; } }
static std::string read_until_close(int fd) { std::string r; char b[4096]; ssize_t n; while ((n = ::recv(fd, b, sizeof b, 0)) > 0) r.append(b, n); return r; }
static std::string read_n(int fd, size_t n) { std::string r; char b[4096]; while (r.size() < n) { ssize_t k = ::recv(fd, b, std::min(sizeof b, n - r.size()), 0); if (k <= 0) break; r.append(b, k); } return r; }
static std::string read_head(int fd) { std::string r; char c; while (r.size() < 8192 && ::recv(fd, &c, 1, 0) == 1) { r += c; if (r.size() >= 4 && r.compare(r.size() - 4, 4, "\r\n\r\n") == 0) break; } return r; }

static std::string masked_frame(uint8_t op, const std::string& p, bool fin = true, bool mask = true) {
    std::string f; f.push_back(char((fin ? 0x80 : 0) | op));
    const uint8_t m[4] = {0x12, 0x34, 0x56, 0x78};
    uint8_t mb = mask ? 0x80 : 0;
    if (p.size() < 126) f.push_back(char(mb | p.size()));
    else { f.push_back(char(mb | 126)); f.push_back(char(p.size() >> 8)); f.push_back(char(p.size() & 0xFF)); }
    if (mask) f.append(reinterpret_cast<const char*>(m), 4);
    for (size_t i = 0; i < p.size(); ++i) f.push_back(mask ? char(p[i] ^ m[i & 3]) : p[i]);
    return f;
}
struct Frame { uint8_t op = 0xFF; std::string payload; };
static Frame read_frame(int fd) {
    Frame f; std::string h = read_n(fd, 2); if (h.size() < 2) return f;
    f.op = uint8_t(h[0]) & 0x0F; size_t len = uint8_t(h[1]) & 0x7F;
    if (len == 126) { std::string e = read_n(fd, 2); len = (uint8_t(e[0]) << 8) | uint8_t(e[1]); }
    f.payload = read_n(fd, len); return f;
}
static std::string ws_handshake(int fd, const std::string& path, const std::string& key = "dGhlIHNhbXBsZSBub25jZQ==") {
    send_all(fd, "GET " + path + " HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\nConnection: keep-alive, Upgrade\r\n"
                 "Sec-WebSocket-Key: " + key + "\r\nSec-WebSocket-Version: 13\r\n\r\n");
    return read_head(fd);
}

int main() {
    // --- primitives (RFC 3174 / RFC 6455 vectors) -----------------------------
    {
        auto h = detail::sha1("abc");
        CHECK(detail::base64(h.data(), h.size()) == "qZk+NkcGgWq6PiVxeFDCbJzQ2J0=");
        CHECK(detail::ws_accept_key("dGhlIHNhbXBsZSBub25jZQ==") == "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=");
        CHECK(detail::base64(reinterpret_cast<const uint8_t*>("f"), 1) == "Zg==");
        CHECK(detail::base64(reinterpret_cast<const uint8_t*>("fo"), 2) == "Zm8=");
        CHECK(detail::base64(reinterpret_cast<const uint8_t*>("foo"), 3) == "Zm9v");
    }

    Options opt; opt.max_body_bytes = 1024; opt.max_ws_message = 4096;
    Server srv(opt);
    srv.get("/hello/:name", [](const Request& r, Response& res) { res.text("hello " + r.params.at("name")); });
    srv.post("/echo", [](const Request& r, Response& res) { res.text(r.body); });
    srv.get("/boom", [](const Request&, Response&) { throw std::runtime_error("boom"); });
    WsHandlers h;
    h.on_message = [](WsConn& c, std::string_view m, bool t) { if (t) c.send_text("echo:" + std::string(m)); else c.send_binary(m); };
    srv.ws("/ws", h);
    const uint16_t port = srv.bind("127.0.0.1", 0);
    std::thread t([&] { srv.run(); });

    // --- HTTP -------------------------------------------------------------------
    {   int fd = dial(port);
        send_all(fd, "GET /hello/world?x=1 HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");
        std::string r = read_until_close(fd);
        CHECK(r.find("200 OK") != std::string::npos); CHECK(r.find("hello world") != std::string::npos); ::close(fd); }
    {   int fd = dial(port);  // keep-alive + pipelining
        send_all(fd, "GET /hello/a HTTP/1.1\r\nHost: x\r\n\r\nGET /hello/b HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");
        std::string r = read_until_close(fd);
        CHECK(r.find("hello a") != std::string::npos); CHECK(r.find("hello b") != std::string::npos); ::close(fd); }
    {   int fd = dial(port);  // body split across packets
        send_all(fd, "POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\nConnection: close\r\n\r\nab");
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        send_all(fd, "cde");
        CHECK(read_until_close(fd).find("abcde") != std::string::npos); ::close(fd); }
    {   int fd = dial(port); send_all(fd, "GET /nope HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");
        CHECK(read_until_close(fd).find("404 Not Found") != std::string::npos); ::close(fd); }
    {   int fd = dial(port); send_all(fd, "POST /hello/x HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");
        CHECK(read_until_close(fd).find("405") != std::string::npos); ::close(fd); }
    {   int fd = dial(port); send_all(fd, "GET /boom HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");
        CHECK(read_until_close(fd).find("500") != std::string::npos); ::close(fd); }
    {   int fd = dial(port); send_all(fd, "POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: 999999\r\n\r\n");
        CHECK(read_until_close(fd).find("413") != std::string::npos); ::close(fd); }
    {   int fd = dial(port); send_all(fd, "garbage\r\n\r\n");
        CHECK(read_until_close(fd).find("400") != std::string::npos); ::close(fd); }
    {   int fd = dial(port); send_all(fd, "POST /echo HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n");
        CHECK(read_until_close(fd).find("501") != std::string::npos); ::close(fd); }
    {   int fd = dial(port); send_all(fd, "GET /x HTTP/1.1\r\nHost: x\r\n" + std::string(20000, 'a') + "\r\n\r\n");
        CHECK(read_until_close(fd).find("431") != std::string::npos); ::close(fd); }

    // --- WebSocket --------------------------------------------------------------
    {   int fd = dial(port);
        std::string head = ws_handshake(fd, "/ws");
        CHECK(head.find("101 Switching Protocols") != std::string::npos);
        CHECK(head.find("s3pPLMBiTxaQ9kYGzzhZRbK+xOo=") != std::string::npos);
        send_all(fd, masked_frame(0x1, "hi"));
        Frame f = read_frame(fd); CHECK(f.op == 0x1 && f.payload == "echo:hi");
        send_all(fd, masked_frame(0x2, std::string("\x00\x01\xff", 3)));
        f = read_frame(fd); CHECK(f.op == 0x2 && f.payload == std::string("\x00\x01\xff", 3));
        send_all(fd, masked_frame(0x9, "p"));
        f = read_frame(fd); CHECK(f.op == 0xA && f.payload == "p");
        send_all(fd, masked_frame(0x1, "foo", false)); send_all(fd, masked_frame(0x0, "bar", true));  // fragmented
        f = read_frame(fd); CHECK(f.op == 0x1 && f.payload == "echo:foobar");
        std::string big(1000, 'x'); send_all(fd, masked_frame(0x1, big));  // 16-bit length
        f = read_frame(fd); CHECK(f.payload == "echo:" + big);
        std::string cl = masked_frame(0x8, std::string("\x03\xe8", 2)); send_all(fd, cl);
        f = read_frame(fd); CHECK(f.op == 0x8); CHECK(read_until_close(fd).empty()); ::close(fd); }
    {   int fd = dial(port); ws_handshake(fd, "/ws");   // unmasked client frame -> 1002
        send_all(fd, masked_frame(0x1, "x", true, false));
        Frame f = read_frame(fd); CHECK(f.op == 0x8 && f.payload.size() >= 2 && uint8_t(f.payload[1]) == 0xEA); ::close(fd); }
    {   int fd = dial(port); ws_handshake(fd, "/ws");   // too big -> 1009
        send_all(fd, masked_frame(0x1, std::string(5000, 'x')).substr(0, 8));
        Frame f = read_frame(fd); CHECK(f.op == 0x8 && f.payload.size() >= 2 && uint8_t(f.payload[1]) == 0xF1); ::close(fd); }
    {   int fd = dial(port); std::string head = ws_handshake(fd, "/nope");  // unknown ws path
        CHECK(head.find("404") != std::string::npos); ::close(fd); }

    srv.stop(); t.join();
    if (g_fail == 0) std::puts("all tests passed");
    return g_fail ? 1 : 0;
}
