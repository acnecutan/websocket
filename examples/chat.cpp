// Minimal WebSocket group chat + a couple of REST routes.
//   g++ -std=c++17 -O2 -Iinclude examples/chat.cpp -o chat && ./chat 8080
#include <nanoserve/nanoserve.hpp>

#include <csignal>
#include <cstdio>
#include <string>

using namespace nanoserve;

static const char* kPage = R"HTML(<!doctype html>
<meta charset="utf-8"><title>nanoserve chat</title>
<style>body{font:16px system-ui;max-width:640px;margin:2rem auto;padding:0 1rem}
#log{height:50vh;overflow:auto;border:1px solid #8884;border-radius:8px;padding:.5rem;margin-bottom:.5rem}
form{display:flex;gap:.5rem}input{flex:1;padding:.5rem}</style>
<h1>nanoserve chat</h1><div id="log"></div>
<form id="f"><input id="m" autocomplete="off" placeholder="Say something…" autofocus><button>Send</button></form>
<script>
const log = document.getElementById('log');
const ws = new WebSocket((location.protocol === 'https:' ? 'wss://' : 'ws://') + location.host + '/ws');
const add = t => { const d = document.createElement('div'); d.textContent = t; log.append(d); log.scrollTop = log.scrollHeight; };
ws.onmessage = e => add(e.data);
ws.onclose = () => add('— disconnected —');
document.getElementById('f').onsubmit = e => { e.preventDefault(); const m = document.getElementById('m'); if (m.value) ws.send(m.value); m.value = ''; };
</script>)HTML";

static Server* g_server = nullptr;

int main(int argc, char** argv) {
    const uint16_t port = argc > 1 ? static_cast<uint16_t>(std::atoi(argv[1])) : 8080;
    Server app;
    g_server = &app;
    std::signal(SIGINT, [](int) { if (g_server) g_server->stop(); });
    std::signal(SIGTERM, [](int) { if (g_server) g_server->stop(); });

    app.get("/", [](const Request&, Response& res) { res.html(kPage); });
    app.get("/api/health", [&app](const Request&, Response& res) {
        res.json("{\"status\":\"ok\",\"connections\":" + std::to_string(app.connection_count()) + "}");
    });
    app.get("/api/hello/:name", [](const Request& req, Response& res) {
        res.text("Hello, " + req.params.at("name") + "!\n");
    });

    WsHandlers chat;
    chat.on_open = [&app](WsConn& c) {
        c.send_text("Welcome! You are user-" + std::to_string(c.id()));
        app.broadcast("/ws", "user-" + std::to_string(c.id()) + " joined", c.id());
    };
    chat.on_message = [&app](WsConn& c, std::string_view msg, bool is_text) {
        if (!is_text) return;
        app.broadcast("/ws", "user-" + std::to_string(c.id()) + ": " + std::string(msg));
    };
    chat.on_close = [&app](WsConn& c) { app.broadcast("/ws", "user-" + std::to_string(c.id()) + " left"); };
    app.ws("/ws", chat);

    std::printf("listening on http://0.0.0.0:%u\n", port);
    try { app.listen("0.0.0.0", port); }
    catch (const std::exception& e) { std::fprintf(stderr, "fatal: %s\n", e.what()); return 1; }
    return 0;
}
