// affineui_tools — protocol client implementation (client half of
// docs/AFFINETOOLS_PROTOCOL.md; server half is src/tools/tools_server.cpp).
//
// Threading: attach() runs the handshake synchronously with bounded
// receive timeouts; after that ONE reader thread owns the socket and
// updates the model under a mutex, bumping an atomic revision the UI
// polls. detach()/dtor closes the socket and joins the thread.

#include "affineui_tools.h"

#include "core/tools/json_reader.h"  // core-internal minimal JSON (src/tools)

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
using socket_t = SOCKET;
static constexpr socket_t kInvalidSocket = INVALID_SOCKET;
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
using socket_t = int;
static constexpr socket_t kInvalidSocket = -1;
#endif

namespace affineui::tools {
namespace json = affineui::detail::json;
namespace fs = std::filesystem;

namespace {

bool sockets_init() noexcept {
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

void close_socket(socket_t s) noexcept {
    if (s == kInvalidSocket) return;
#if defined(_WIN32)
    closesocket(s);
#else
    close(s);
#endif
}

void set_recv_timeout(socket_t s, int ms) noexcept {
#if defined(_WIN32)
    const DWORD v = static_cast<DWORD>(ms);
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO,
               reinterpret_cast<const char*>(&v), sizeof(v));
#else
    timeval tv{ms / 1000, (ms % 1000) * 1000};
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif
}

fs::path discovery_dir() {
    std::error_code ec;
    fs::path tmp = fs::temp_directory_path(ec);
    if (ec) tmp = ".";
    return tmp / "affineui-tools";
}

bool send_all(socket_t s, const char* data, std::size_t len) {
    while (len > 0) {
        const int n = ::send(s, data, static_cast<int>(len), 0);
        if (n <= 0) return false;
        data += n;
        len -= static_cast<std::size_t>(n);
    }
    return true;
}

bool send_framed(socket_t s, std::string_view payload) {
    const std::uint32_t len = static_cast<std::uint32_t>(payload.size());
    char hdr[4] = {static_cast<char>(len & 0xFFu),
                   static_cast<char>((len >> 8) & 0xFFu),
                   static_cast<char>((len >> 16) & 0xFFu),
                   static_cast<char>((len >> 24) & 0xFFu)};
    return send_all(s, hdr, 4) && send_all(s, payload.data(), payload.size());
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

/// One framed message; empty on close/timeout/bad length.
std::string recv_framed(socket_t s) {
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

void frame_from_json(const json::Value& params, FrameTelemetry& t) {
    t.v = static_cast<std::uint32_t>(params.get_number("v", 1));
    t.frame = static_cast<std::uint64_t>(params.get_number("frame"));
    t.t_ms = params.get_number("t_ms");
    t.gap_ms = params.get_number("gap_ms");
    t.cb_ms = params.get_number("cb_ms");
    t.skipped = static_cast<std::uint64_t>(params.get_number("skipped"));
    if (const json::Value* fb = params.get("fb");
        fb != nullptr && fb->array.size() == 2) {
        t.fb_w = static_cast<int>(fb->array[0].number);
        t.fb_h = static_cast<int>(fb->array[1].number);
    }
    t.dpi = static_cast<float>(params.get_number("dpi"));
    if (const json::Value* stage = params.get("stage_us")) {
        t.prep_us = static_cast<std::uint32_t>(stage->get_number("prep"));
        t.layout_us = static_cast<std::uint32_t>(stage->get_number("layout"));
        t.record_us = static_cast<std::uint32_t>(stage->get_number("dl"));
        t.raster_us = static_cast<std::uint32_t>(stage->get_number("rast"));
        t.composite_us = static_cast<std::uint32_t>(stage->get_number("comp"));
    }
    if (const json::Value* ops = params.get("ops")) {
        t.cached_ops = static_cast<std::uint32_t>(ops->get_number("cached"));
        t.culled_ops = static_cast<std::uint32_t>(ops->get_number("culled"));
        t.changed_ops = static_cast<std::uint32_t>(ops->get_number("changed"));
        t.dirty_rects = static_cast<std::uint32_t>(ops->get_number("rects"));
        t.dirty_pct_x100 =
            static_cast<std::uint32_t>(ops->get_number("dirty_pct") * 100.0);
    }
    if (const json::Value* mem = params.get("mem")) {
        t.mem_live_bytes = static_cast<std::uint64_t>(mem->get_number("live"));
        t.mem_live_blocks =
            static_cast<std::uint64_t>(mem->get_number("blocks"));
        t.allocs = static_cast<std::uint32_t>(mem->get_number("allocs"));
        t.frees = static_cast<std::uint32_t>(mem->get_number("frees"));
    }
}

}  // namespace

std::vector<TargetInfo> discover_targets() {
    std::vector<std::pair<fs::file_time_type, TargetInfo>> found;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(discovery_dir(), ec)) {
        if (entry.path().extension() != ".json") continue;
        std::ifstream f(entry.path(), std::ios::binary);
        if (!f.good()) continue;
        std::string text((std::istreambuf_iterator<char>(f)),
                         std::istreambuf_iterator<char>());
        json::Value v;
        if (!json::parse(text, v) || !v.is_object()) continue;
        TargetInfo t;
        t.pid = static_cast<int>(v.get_number("pid"));
        t.port = static_cast<int>(v.get_number("port"));
        t.token = std::string(v.get_string("token"));
        t.exe = std::string(v.get_string("exe"));
        t.affineui_version = std::string(v.get_string("affineui"));
        if (t.port <= 0 || t.token.empty()) continue;
        found.emplace_back(entry.last_write_time(ec), std::move(t));
    }
    std::sort(found.begin(), found.end(),
              [](const auto& a, const auto& b) { return a.first > b.first; });
    std::vector<TargetInfo> out;
    out.reserve(found.size());
    for (auto& [_, t] : found) out.push_back(std::move(t));
    return out;
}

struct Client::Impl {
    socket_t sock{kInvalidSocket};
    std::thread reader;
    std::atomic<bool> running{false};
    std::atomic<std::uint64_t> revision{0};
    mutable std::mutex mutex;
    ClientStatus model;
    std::deque<FrameTelemetry> history;  // recent frames (scrolling graph)
    static constexpr std::size_t kHistoryMax = 512;
    std::deque<LogEntry> logs;            // recent log lines (log panel)
    static constexpr std::size_t kLogMax = 4096;  // client-side log budget
    std::size_t   log_total{0};           // lines received this session
    std::uint64_t log_dropped_total{0};   // budget/ring drops (target + client)
    int next_id{1};

    bool request(const std::string& method, const std::string& params_json,
                 json::Value* out_result) {
        char head[96];
        const int id = next_id++;
        std::string msg;
        if (params_json.empty()) {
            std::snprintf(head, sizeof(head), "{\"id\":%d,\"method\":\"%s\"}",
                          id, method.c_str());
            msg = head;
        } else {
            std::snprintf(head, sizeof(head),
                          "{\"id\":%d,\"method\":\"%s\",\"params\":", id,
                          method.c_str());
            msg = head;
            msg += params_json;
            msg += '}';
        }
        if (!send_framed(sock, msg)) return false;
        // Events may interleave with the response — skip them here (the
        // reader thread isn't running yet during the handshake).
        for (int attempts = 0; attempts < 64; ++attempts) {
            const std::string reply = recv_framed(sock);
            if (reply.empty()) return false;
            json::Value v;
            if (!json::parse(reply, v) || !v.is_object()) return false;
            if (static_cast<int>(v.get_number("id", -1)) != id) continue;
            if (v.get("error") != nullptr) return false;
            if (out_result != nullptr) {
                if (const json::Value* r = v.get("result")) *out_result = *r;
            }
            return true;
        }
        return false;
    }

    void handle_event(const json::Value& v) {
        const std::string_view method = v.get_string("method");
        const json::Value* params = v.get("params");
        const std::lock_guard<std::mutex> lock(mutex);
        model.events += 1;
        if (method == "telemetry.frame" && params != nullptr) {
            frame_from_json(*params, model.last_frame);
            model.have_frame = true;
            model.last_was_idle = false;
            history.push_back(model.last_frame);
            if (history.size() > kHistoryMax) history.pop_front();
        } else if (method == "target.idle" && params != nullptr) {
            model.idle_t_ms = params->get_number("t_ms");
            model.idle_skipped =
                static_cast<std::uint64_t>(params->get_number("skipped"));
            model.last_was_idle = true;
        } else if (method == "telemetry.dropped" && params != nullptr) {
            model.dropped +=
                static_cast<std::uint64_t>(params->get_number("count"));
        } else if (method == "log.line" && params != nullptr) {
            LogEntry e;
            const std::string_view lvl = params->get_string("level");
            e.level = lvl == "debug" ? 0 : lvl == "warn" ? 2
                      : lvl == "error"                    ? 3
                                                          : 1;
            e.frame = static_cast<std::uint64_t>(params->get_number("frame"));
            e.t_ms = params->get_number("t_ms");
            e.text = std::string(params->get_string("text"));
            logs.push_back(std::move(e));
            ++log_total;
            if (logs.size() > kLogMax) {
                logs.pop_front();
                ++log_dropped_total;
            }
        } else if (method == "log.dropped" && params != nullptr) {
            log_dropped_total +=
                static_cast<std::uint64_t>(params->get_number("count"));
        }
    }

    void reader_loop() {
        while (running.load(std::memory_order_acquire)) {
            const std::string msg = recv_framed(sock);
            if (msg.empty()) {
                // Timeout tick or closed. Distinguish: a closed socket
                // keeps returning empty instantly; probe with running.
                if (!running.load(std::memory_order_acquire)) break;
                // recv timeout (idle target between heartbeats) — poll on.
                // A dead connection also lands here eventually; the next
                // send from the target side is gone, so detect via a
                // zero-byte recv happening immediately many times is not
                // distinguishable cheaply — acceptable for S1.
                continue;
            }
            json::Value v;
            if (!json::parse(msg, v) || !v.is_object()) continue;
            if (v.get("method") != nullptr) {
                handle_event(v);
                revision.fetch_add(1, std::memory_order_release);
            }
        }
    }
};

Client::Client() : impl_{std::make_unique<Impl>()} {}

Client::~Client() { detach(); }

bool Client::attach(const TargetInfo& target) {
    detach();
    if (!sockets_init()) return false;

    socket_t s = socket(AF_INET, SOCK_STREAM, 0);
    if (s == kInvalidSocket) return false;
    set_recv_timeout(s, 500);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(static_cast<std::uint16_t>(target.port));
    if (connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        close_socket(s);
        return false;
    }
    impl_->sock = s;

    std::string params = "{\"token\":\"";
    params += target.token;  // hex — no escaping needed
    params += "\",\"client\":\"affinetools\"}";
    json::Value hello;
    if (!impl_->request("hello", params, &hello) ||
        !impl_->request("telemetry.subscribe", {}, nullptr) ||
        !impl_->request("log.subscribe", {}, nullptr)) {
        close_socket(impl_->sock);
        impl_->sock = kInvalidSocket;
        return false;
    }

    {
        const std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->model = ClientStatus{};
        impl_->history.clear();
        impl_->logs.clear();
        impl_->log_total = 0;
        impl_->log_dropped_total = 0;
        impl_->model.connected = true;
        impl_->model.pid = target.pid;
        impl_->model.exe = target.exe;
        impl_->model.affineui_version = std::string(hello.get_string("affineui"));
        impl_->model.session_id = std::string(hello.get_string("session_id"));
    }
    impl_->running.store(true, std::memory_order_release);
    impl_->reader = std::thread([this] { impl_->reader_loop(); });
    impl_->revision.fetch_add(1, std::memory_order_release);
    return true;
}

bool Client::attach_pid(int pid) {
    for (const TargetInfo& t : discover_targets()) {
        if (t.pid == pid) return attach(t);
    }
    return false;
}

bool Client::attach_newest() {
    for (const TargetInfo& t : discover_targets()) {
        if (attach(t)) return true;
    }
    return false;
}

void Client::detach() {
    if (!impl_) return;
    impl_->running.store(false, std::memory_order_release);
    if (impl_->sock != kInvalidSocket) {
        close_socket(impl_->sock);  // unblocks the reader's recv
        impl_->sock = kInvalidSocket;
    }
    if (impl_->reader.joinable()) impl_->reader.join();
    const std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->model.connected = false;
}

bool Client::attached() const noexcept {
    const std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->model.connected;
}

std::uint64_t Client::revision() const noexcept {
    return impl_->revision.load(std::memory_order_acquire);
}

ClientStatus Client::status() const {
    const std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->model;
}

void Client::frame_history(std::vector<FrameTelemetry>& out,
                           std::size_t max) const {
    const std::lock_guard<std::mutex> lock(impl_->mutex);
    const std::size_t n = std::min(max, impl_->history.size());
    out.clear();
    out.reserve(n);
    for (std::size_t i = impl_->history.size() - n; i < impl_->history.size();
         ++i) {
        out.push_back(impl_->history[i]);
    }
}

void Client::log_history(std::vector<LogEntry>& out, std::size_t max) const {
    const std::lock_guard<std::mutex> lock(impl_->mutex);
    const std::size_t n = std::min(max, impl_->logs.size());
    out.clear();
    out.reserve(n);
    for (std::size_t i = impl_->logs.size() - n; i < impl_->logs.size(); ++i) {
        out.push_back(impl_->logs[i]);
    }
}

std::size_t Client::log_count() const noexcept {
    const std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->logs.size();
}

std::uint64_t Client::log_dropped() const noexcept {
    const std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->log_dropped_total;
}

RangeStats compute_range_stats(const std::vector<FrameTelemetry>& frames,
                               std::size_t begin, std::size_t end,
                               double budget_ms) {
    RangeStats out;
    out.budget_ms = budget_ms;
    end = std::min(end, frames.size());
    if (begin >= end) return out;

    const std::size_t n = end - begin;
    out.frames = n;
    double sum_gap = 0.0, sum_cb = 0.0;
    double sum_layout = 0.0, sum_raster = 0.0, sum_comp = 0.0, sum_other = 0.0;
    out.min_gap_ms = frames[begin].gap_ms;
    out.max_gap_ms = frames[begin].gap_ms;
    std::vector<double> gaps;
    gaps.reserve(n);
    for (std::size_t i = begin; i < end; ++i) {
        const FrameTelemetry& f = frames[i];
        sum_gap += f.gap_ms;
        sum_cb += f.cb_ms;
        out.min_gap_ms = std::min(out.min_gap_ms, f.gap_ms);
        out.max_gap_ms = std::max(out.max_gap_ms, f.gap_ms);
        if (f.gap_ms > budget_ms) ++out.over_budget;
        const double layout = f.layout_us / 1000.0;
        const double raster = f.raster_us / 1000.0;
        const double comp = f.composite_us / 1000.0;
        sum_layout += layout;
        sum_raster += raster;
        sum_comp += comp;
        sum_other += std::max(0.0, f.gap_ms - (layout + raster + comp));
        gaps.push_back(f.gap_ms);
    }
    const double dn = static_cast<double>(n);
    out.avg_gap_ms = sum_gap / dn;
    out.avg_cb_ms = sum_cb / dn;
    out.avg_layout_ms = sum_layout / dn;
    out.avg_raster_ms = sum_raster / dn;
    out.avg_composite_ms = sum_comp / dn;
    out.avg_other_ms = sum_other / dn;
    out.avg_fps = out.avg_gap_ms > 0.0 ? 1000.0 / out.avg_gap_ms : 0.0;
    std::sort(gaps.begin(), gaps.end());
    out.p95_gap_ms = gaps[(gaps.size() * 95) / 100 >= gaps.size()
                              ? gaps.size() - 1
                              : (gaps.size() * 95) / 100];
    return out;
}

DumpSummary summarize_dump(const std::string& path) {
    DumpSummary out;
    std::ifstream f(path, std::ios::binary);
    if (!f.good()) {
        out.error = "cannot open file";
        return out;
    }
    std::vector<double> gaps;
    double cb_sum = 0.0;
    std::string line;
    while (std::getline(f, line)) {
        if (line.empty()) continue;
        json::Value v;
        if (!json::parse(line, v) || !v.is_object()) continue;
        const std::string_view type = v.get_string("type");
        if (type == "session") {
            out.affineui_version = std::string(v.get_string("affineui"));
        } else if (type == "frame") {
            gaps.push_back(v.get_number("gap_ms"));
            cb_sum += v.get_number("cb_ms");
            out.frames += 1;
        } else if (type == "idle") {
            out.idles += 1;
        }
    }
    if (!gaps.empty()) {
        std::sort(gaps.begin(), gaps.end());
        double sum = 0.0;
        for (const double g : gaps) sum += g;
        out.avg_gap_ms = sum / static_cast<double>(gaps.size());
        out.p95_gap_ms = gaps[gaps.size() * 95 / 100];
        out.max_gap_ms = gaps.back();
        out.avg_cb_ms = cb_sum / static_cast<double>(gaps.size());
    }
    out.ok = out.frames > 0 || out.idles > 0;
    if (!out.ok) out.error = "no telemetry records in file";
    return out;
}

}  // namespace affineui::tools
