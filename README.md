# nanoserve

**A tiny HTTP/1.1 + WebSocket server for Linux in one C++17 header.**
No dependencies, no build system required, ~700 lines you can read in an evening.

```cpp
#include <nanoserve/nanoserve.hpp>
using namespace nanoserve;

int main() {
    Server app;
    app.get("/hello/:name", [](const Request& req, Response& res) {
        res.text("Hello, " + req.params.at("name") + "!\n");
    });

    WsHandlers chat;
    chat.on_message = [&](WsConn& c, std::string_view msg, bool) {
        app.broadcast("/ws", "user-" + std::to_string(c.id()) + ": " + std::string(msg));
    };
    app.ws("/ws", chat);

    app.listen("0.0.0.0", 8080);
}
```

```bash
g++ -std=c++17 -O2 -Iinclude examples/chat.cpp -o chat && ./chat 8080
# open http://localhost:8080 in two tabs
```

## Why

Most C++ web libraries are either huge (Boost.Beast, Drogon) or pull in a build system and half a dozen
dependencies. nanoserve is the opposite: copy one file, `#include` it, ship. It is also deliberately small
enough to be **a readable reference for how an epoll event loop, an HTTP parser and RFC 6455 framing fit together.**

## Features

- Non-blocking `epoll` event loop (level-triggered), single thread, no locks
- HTTP/1.1: keep-alive, pipelining, `Content-Length` bodies, path params (`/users/:id`), query string, 404/405/500 handling
- WebSocket (RFC 6455): handshake, text/binary, fragmentation, ping/pong, close handshake with status codes, server-side keep-alive pings
- `broadcast()` by path for rooms/channels
- Backpressure: slow consumers are disconnected once their write buffer exceeds a limit
- Hardening: header/body/message size limits, idle timeouts, connection cap, header-injection guard, strict masking and reserved-bit checks
- Handler exceptions never crash the loop (HTTP → 500, WebSocket → logged)
- Tested under ASan/UBSan (see `tests/`), including RFC 6455 handshake vectors

## API

| | |
|---|---|
| `Server app(Options{})` | limits and timeouts are in `Options` |
| `app.get/post/put/del(pattern, handler)` | `handler(const Request&, Response&)` |
| `app.ws(path, WsHandlers{on_open,on_message,on_close})` | `WsConn&` has `id()`, `send_text()`, `send_binary()`, `close()` |
| `app.broadcast(path, text, exclude_id)` | send to every open socket on a path |
| `app.send_text(id, text)` | message a connection by id |
| `app.bind(host, port)` / `app.run()` / `app.listen(host, port)` | `bind` with port `0` picks a free port and returns it |
| `app.stop()` | safe from another thread or a signal handler |

## Build & test

```bash
# tests (with sanitizers)
g++ -std=c++17 -g -O1 -fsanitize=address,undefined -Iinclude tests/test_server.cpp -o test_server -pthread && ./test_server

# or with CMake
cmake -B build && cmake --build build && ctest --test-dir build --output-on-failure
```

## Limitations (read before using in production)

- **Linux only** (epoll). IPv4 only.
- **No TLS.** Put it behind Caddy, nginx or HAProxy for HTTPS/WSS.
- Single-threaded: handlers must not block. Scale by running several processes behind a load balancer.
- Request bodies must use `Content-Length`; `Transfer-Encoding: chunked` requests get `501`.
- Text frames are **not** validated as UTF-8; no `permessage-deflate`; no HTTP/2.
- Not yet run against the Autobahn test suite — contributions welcome.
- No static-file serving by design (avoids path-traversal footguns); serve assets from a CDN or a reverse proxy.

## Roadmap

- [ ] Autobahn `fuzzingclient` run in CI
- [ ] UTF-8 validation for text frames
- [ ] Optional multi-reactor mode (`SO_REUSEPORT`)
- [ ] Chunked request bodies
- [ ] IPv6

## License

MIT
