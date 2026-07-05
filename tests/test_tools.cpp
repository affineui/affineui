// affinetools protocol server: JSON reader hardening + a live loopback
// integration test — listen → discovery file → hello (bad token rejected,
// good token authed) → subscribe → push_frame → framed telemetry.frame
// event → shutdown. This is the S1 spine exercised end-to-end in-process;
// scripts/affinetools_cli.py is the same flow from Python.

#include "affineui/renderer.h"
#include "affineui/telemetry.h"
#include "affineui/tools.h"
#include "tools/json_reader.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ostream>
#include <string>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <process.h>
using socket_t = SOCKET;
static constexpr socket_t kBadSocket = INVALID_SOCKET;
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
using socket_t = int;
static constexpr socket_t kBadSocket = -1;
#endif

using namespace affineui;
namespace json = affineui::detail::json;

// ── JSON reader ──────────────────────────────────────────────────────────

TEST_CASE("json reader: objects, arrays, strings, numbers") {
    // NOTE: pure-ASCII source, and JSON escapes spelled with doubled
    // backslashes in ordinary literals — MSVC scans \u sequences even
    // inside raw strings, so R"(...é...)" is not portable.
    json::Value v;
    REQUIRE(json::parse(
        "{\"id\":7,\"method\":\"hello\",\"params\":{\"token\":\"abc\","
        "\"n\":-1.5e2,\"flags\":[true,false,null],"
        "\"esc\":\"a\\\"b\\\\c\\nd\"}}",
        v));
    CHECK(v.is_object());
    CHECK(v.get_number("id") == 7.0);
    CHECK(v.get_string("method") == "hello");
    const json::Value* params = v.get("params");
    REQUIRE(params != nullptr);
    CHECK(params->get_string("token") == "abc");
    CHECK(params->get_number("n") == -150.0);
    const json::Value* flags = params->get("flags");
    REQUIRE(flags != nullptr);
    REQUIRE(flags->array.size() == 3);
    CHECK(flags->array[0].boolean);
    CHECK_FALSE(flags->array[1].boolean);
    CHECK(flags->array[2].kind == json::Value::Kind::Null);
    CHECK(params->get_string("esc") == "a\"b\\c\nd");
}

TEST_CASE("json reader: unicode escapes decode to UTF-8") {
    json::Value v;
    // U+00E9 (e-acute) and the surrogate pair for U+1F600 (emoji).
    REQUIRE(json::parse("{\"e\":\"\\u00e9\",\"s\":\"\\ud83d\\ude00\"}", v));
    CHECK(v.get_string("e") == "\xC3\xA9");
    CHECK(v.get_string("s") == "\xF0\x9F\x98\x80");
}

TEST_CASE("json reader: malformed input fails closed") {
    json::Value v;
    CHECK_FALSE(json::parse("", v));
    CHECK_FALSE(json::parse("{", v));
    CHECK_FALSE(json::parse("{}extra", v));
    CHECK_FALSE(json::parse(R"({"a":})", v));
    CHECK_FALSE(json::parse(R"({"a":1,})", v));
    CHECK_FALSE(json::parse("\"unterminated", v));
    CHECK_FALSE(json::parse("\"ctrl\x01char\"", v));
    CHECK_FALSE(json::parse("\"\\ud800 lone high\"", v));
    CHECK_FALSE(json::parse("\"\\udc00\"", v));
    CHECK_FALSE(json::parse("01", v));       // leading zero
    CHECK_FALSE(json::parse("1.", v));       // bare decimal point
    CHECK_FALSE(json::parse("tru", v));
    // Depth bomb: 64 nested arrays > max_depth 32.
    std::string deep(64, '[');
    deep += std::string(64, ']');
    CHECK_FALSE(json::parse(deep, v));
    // ...but 16 deep is fine.
    std::string ok(16, '[');
    ok += "1";
    ok += std::string(16, ']');
    json::Value v2;
    CHECK(json::parse(ok, v2));
}

// ── live server integration ──────────────────────────────────────────────

namespace {

bool test_sockets_init() {
#if defined(_WIN32)
    static const bool ok = [] {
        WSADATA wsa{};
        return WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
    }();
    return ok;
#else
    return true;
#endif
}

void test_close(socket_t s) {
#if defined(_WIN32)
    if (s != kBadSocket) closesocket(s);
#else
    if (s != kBadSocket) close(s);
#endif
}

socket_t connect_loopback(int port) {
    socket_t s = socket(AF_INET, SOCK_STREAM, 0);
    if (s == kBadSocket) return s;
    // Bounded reads so a hung server fails the test instead of wedging it.
#if defined(_WIN32)
    const DWORD timeout_ms = 3000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO,
               reinterpret_cast<const char*>(&timeout_ms), sizeof(timeout_ms));
#else
    timeval timeout{3, 0};
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
#endif
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(static_cast<std::uint16_t>(port));
    if (connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        test_close(s);
        return kBadSocket;
    }
    return s;
}

bool send_framed_msg(socket_t s, const std::string& payload) {
    const std::uint32_t len = static_cast<std::uint32_t>(payload.size());
    char hdr[4] = {static_cast<char>(len & 0xFFu),
                   static_cast<char>((len >> 8) & 0xFFu),
                   static_cast<char>((len >> 16) & 0xFFu),
                   static_cast<char>((len >> 24) & 0xFFu)};
    std::string wire(hdr, 4);
    wire += payload;
    std::size_t off = 0;
    while (off < wire.size()) {
        const int n = send(s, wire.data() + off,
                           static_cast<int>(wire.size() - off), 0);
        if (n <= 0) return false;
        off += static_cast<std::size_t>(n);
    }
    return true;
}

bool recv_exact(socket_t s, char* out, std::size_t len) {
    std::size_t off = 0;
    while (off < len) {
        const int n = recv(s, out + off, static_cast<int>(len - off), 0);
        if (n <= 0) return false;  // closed or timed out
        off += static_cast<std::size_t>(n);
    }
    return true;
}

/// Receive one framed message; empty string on close/timeout.
std::string recv_framed_msg(socket_t s) {
    char hdr[4];
    if (!recv_exact(s, hdr, 4)) return {};
    const unsigned char* p = reinterpret_cast<const unsigned char*>(hdr);
    const std::uint32_t len = static_cast<std::uint32_t>(p[0]) |
                              (static_cast<std::uint32_t>(p[1]) << 8) |
                              (static_cast<std::uint32_t>(p[2]) << 16) |
                              (static_cast<std::uint32_t>(p[3]) << 24);
    if (len == 0 || len > (1u << 20)) return {};
    std::string payload(len, '\0');
    if (!recv_exact(s, payload.data(), len)) return {};
    return payload;
}

std::string read_discovery_file() {
    char path[512];
#if defined(_WIN32)
    char tmp[400];
    const DWORD n = GetTempPathA(sizeof(tmp), tmp);
    if (n == 0 || n >= sizeof(tmp)) return {};
    std::snprintf(path, sizeof(path), "%saffineui-tools\\%d.json", tmp,
                  _getpid());
#else
    const char* tmp = std::getenv("TMPDIR");
    std::snprintf(path, sizeof(path), "%s/affineui-tools/%d.json",
                  (tmp != nullptr && tmp[0] != '\0') ? tmp : "/tmp",
                  static_cast<int>(getpid()));
#endif
    FILE* f = nullptr;
#if defined(_WIN32)
    if (fopen_s(&f, path, "r") != 0) f = nullptr;
#else
    f = std::fopen(path, "r");
#endif
    if (!f) return {};
    char buf[1024];
    const std::size_t got = std::fread(buf, 1, sizeof(buf), f);
    std::fclose(f);
    return std::string(buf, got);
}

}  // namespace

TEST_CASE("tools: attached app-thread cost stays far under the 100us budget") {
    // DESIGN §2.2 / §6 item 7: while attached, the app thread's per-frame
    // tools work is telemetry assembly + one ring push (formatting and
    // socket IO happen on the server thread). Measure that app-thread
    // slice. Median-of-batches so background load can't flake it; the
    // budget is 100 us — expected reality is well under 1 us.
    RenderStats rs{};
    rs.cached_ops = 130;
    rs.prepare_us_this_frame = 12;
    const mem::Stats m0 = mem::stats();

    constexpr int kBatches = 9;
    constexpr int kPerBatch = 2000;
    double batch_us[kBatches];
    FrameTelemetry t{};
    for (int b = 0; b < kBatches; ++b) {
        const auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < kPerBatch; ++i) {
            t.frame = static_cast<std::uint64_t>(b) * kPerBatch +
                      static_cast<std::uint64_t>(i);
            t.gap_ms = 6.9;
            fill_from_render_stats(t, rs);
            fill_mem_delta(t, m0, m0);
            tools::push_frame(t);  // no server running: worst case is the
                                   // same lock + copy; ring just wraps
        }
        const auto t1 = std::chrono::steady_clock::now();
        batch_us[b] =
            std::chrono::duration<double, std::micro>(t1 - t0).count() /
            kPerBatch;
    }
    std::sort(batch_us, batch_us + kBatches);
    const double median_us = batch_us[kBatches / 2];
    MESSAGE("attached app-thread cost/frame: median "
            << median_us << " us (budget 100 us)");
    CHECK(median_us < 100.0);

    // Server-thread formatting cost, informational (not app-thread budget).
    char buf[512];
    const auto f0 = std::chrono::steady_clock::now();
    for (int i = 0; i < kPerBatch; ++i) {
        (void) format_frame_jsonl(t, buf, sizeof(buf));
    }
    const auto f1 = std::chrono::steady_clock::now();
    MESSAGE("server-thread format cost/record: "
            << std::chrono::duration<double, std::micro>(f1 - f0).count() /
                   kPerBatch
            << " us");
}

TEST_CASE("tools server: discovery, auth, subscribe, telemetry event") {
    REQUIRE(test_sockets_init());
    REQUIRE(tools_listen(0));
    REQUIRE(tools_active());
    const int port = tools_port();
    REQUIRE(port > 0);

    // Discovery file: parseable JSON carrying the bound port + token.
    const std::string disco_text = read_discovery_file();
    REQUIRE_FALSE(disco_text.empty());
    json::Value disco;
    REQUIRE(json::parse(disco_text, disco));
    CHECK(static_cast<int>(disco.get_number("port")) == port);
    const std::string token{disco.get_string("token")};
    REQUIRE(token.size() == 32);

    // Wrong token → server drops the connection without answering.
    {
        socket_t s = connect_loopback(port);
        REQUIRE(s != kBadSocket);
        REQUIRE(send_framed_msg(
            s, R"({"id":1,"method":"hello","params":{"token":"wrong"}})"));
        CHECK(recv_framed_msg(s).empty());
        test_close(s);
    }

    // Real handshake.
    socket_t s = connect_loopback(port);
    REQUIRE(s != kBadSocket);
    REQUIRE(send_framed_msg(
        s, std::string(R"({"id":2,"method":"hello","params":{"token":")") +
               token + R"("}})"));
    {
        const std::string reply = recv_framed_msg(s);
        REQUIRE_FALSE(reply.empty());
        json::Value v;
        REQUIRE(json::parse(reply, v));
        CHECK(v.get_number("id") == 2.0);
        const json::Value* result = v.get("result");
        REQUIRE(result != nullptr);
        CHECK(result->get_number("protocol") == 0.0);
        CHECK(result->get_string("session_id").size() == 16);
        CHECK_FALSE(result->get_string("affineui").empty());
    }

    // Pre-auth-only methods now work; unknown methods error politely.
    REQUIRE(send_framed_msg(s, R"({"id":3,"method":"nope"})"));
    {
        json::Value v;
        REQUIRE(json::parse(recv_framed_msg(s), v));
        CHECK(v.get("error") != nullptr);
    }

    // Subscribe, then simulate the app thread presenting a frame.
    REQUIRE(send_framed_msg(s, R"({"id":4,"method":"telemetry.subscribe"})"));
    {
        json::Value v;
        REQUIRE(json::parse(recv_framed_msg(s), v));
        CHECK(v.get("result") != nullptr);
    }
    CHECK(tools::wants_telemetry());

    FrameTelemetry t{};
    t.frame = 4242;
    t.gap_ms = 7.11;
    t.fb_w = 640;
    t.fb_h = 480;
    tools::push_frame(t);
    {
        const std::string event = recv_framed_msg(s);
        REQUIRE_FALSE(event.empty());
        json::Value v;
        REQUIRE(json::parse(event, v));
        CHECK(v.get_string("method") == "telemetry.frame");
        const json::Value* params = v.get("params");
        REQUIRE(params != nullptr);
        CHECK(params->get_number("frame") == 4242.0);
        CHECK(params->get_string("type") == "frame");
    }

    // Idle heartbeat rides the same channel.
    tools::push_idle(1234.5, 99);
    {
        json::Value v;
        REQUIRE(json::parse(recv_framed_msg(s), v));
        CHECK(v.get_string("method") == "target.idle");
        const json::Value* params = v.get("params");
        REQUIRE(params != nullptr);
        CHECK(params->get_number("skipped") == 99.0);
    }

    test_close(s);
    tools_shutdown();
    CHECK_FALSE(tools_active());
    CHECK(tools_port() == 0);
    CHECK(read_discovery_file().empty());  // removed on shutdown
}
