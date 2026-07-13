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
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>
#include <unordered_map>

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

/// Decode a wire `"nid":[doc,slot,gen]` array.
DomHandle nid_from_json(const json::Value* arr) {
    DomHandle out{};
    if (arr == nullptr || arr->array.size() != 3) return out;
    out.document_id = static_cast<std::uint32_t>(arr->array[0].number);
    out.node_slot = static_cast<std::uint32_t>(arr->array[1].number);
    out.generation = static_cast<std::uint32_t>(arr->array[2].number);
    return out;
}

Rect rect_from_json(const json::Value* arr) {
    Rect out{};
    if (arr == nullptr || arr->array.size() != 4) return out;
    out.x = static_cast<int>(arr->array[0].number);
    out.y = static_cast<int>(arr->array[1].number);
    out.w = static_cast<int>(arr->array[2].number);
    out.h = static_cast<int>(arr->array[3].number);
    return out;
}

void node_from_json(const json::Value& v, DomNodeInfo& out) {
    out = DomNodeInfo{};
    out.nid = nid_from_json(v.get("nid"));
    out.tag = std::string(v.get_string("tag"));
    out.text = std::string(v.get_string("text"));
    out.child_count = static_cast<int>(v.get_number("children"));
    out.rect = rect_from_json(v.get("rect"));
    if (const json::Value* attrs = v.get("attrs")) {
        out.attrs.reserve(attrs->array.size());
        for (const json::Value& pair : attrs->array) {
            if (pair.array.size() != 2) continue;
            out.attrs.emplace_back(pair.array[0].string,
                                   pair.array[1].string);
        }
    }
}

void nid_params(std::string& out, const DomHandle& nid) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "{\"nid\":[%u,%u,%u]}", nid.document_id,
                  nid.node_slot, nid.generation);
    out = buf;
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

    // Post-attach request/response: the caller registers a pending slot
    // keyed by request id, the reader thread fulfills it (responses
    // interleave with events on the one socket), and the caller waits on
    // the condvar with a timeout.
    struct PendingRequest {
        bool        done{false};
        bool        ok{false};
        json::Value result;
    };
    std::mutex                                 req_mutex;
    std::condition_variable                    req_cv;
    std::unordered_map<int, PendingRequest*>   pending;

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

    /// Fulfill a pending wire request from a response message (reader
    /// thread). Unmatched ids (a request that already timed out) are
    /// dropped silently.
    void handle_response(json::Value& v) {
        const int id = static_cast<int>(v.get_number("id", -1));
        const std::lock_guard<std::mutex> lock(req_mutex);
        const auto it = pending.find(id);
        if (it == pending.end()) return;
        PendingRequest& slot = *it->second;
        slot.ok = v.get("error") == nullptr;
        if (slot.ok) {
            if (json::Value* r = const_cast<json::Value*>(v.get("result"))) {
                slot.result = std::move(*r);
            }
        }
        slot.done = true;
        req_cv.notify_all();
    }

    /// Send a request and wait for its response (post-attach path; the
    /// reader thread owns the socket reads). Returns false on timeout,
    /// disconnect, or a wire error.
    bool wire_request(const char* method, const std::string& params_json,
                      json::Value* out_result, int timeout_ms = 2000) {
        if (sock == kInvalidSocket ||
            !running.load(std::memory_order_acquire)) {
            return false;
        }
        PendingRequest slot;
        int id = 0;
        {
            const std::lock_guard<std::mutex> lock(req_mutex);
            id = next_id++;
            pending.emplace(id, &slot);
        }
        char head[96];
        std::string msg;
        if (params_json.empty()) {
            std::snprintf(head, sizeof(head), "{\"id\":%d,\"method\":\"%s\"}",
                          id, method);
            msg = head;
        } else {
            std::snprintf(head, sizeof(head),
                          "{\"id\":%d,\"method\":\"%s\",\"params\":", id,
                          method);
            msg = head;
            msg += params_json;
            msg += '}';
        }
        const bool sent = send_framed(sock, msg);
        std::unique_lock<std::mutex> lock(req_mutex);
        if (sent) {
            req_cv.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                            [&slot] { return slot.done; });
        }
        pending.erase(id);
        if (!slot.done || !slot.ok) return false;
        if (out_result != nullptr) *out_result = std::move(slot.result);
        return true;
    }

    /// Wake every waiter with a failure (detach / connection loss).
    void fail_pending() {
        const std::lock_guard<std::mutex> lock(req_mutex);
        for (auto& [id, slot] : pending) {
            slot->done = true;
            slot->ok = false;
        }
        req_cv.notify_all();
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
            } else if (v.get("id") != nullptr) {
                handle_response(v);
            }
        }
        fail_pending();
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
#if defined(_WIN32)
    const int self = static_cast<int>(GetCurrentProcessId());
#else
    const int self = static_cast<int>(getpid());
#endif
    for (const TargetInfo& t : discover_targets()) {
        // Never auto-attach to our own process (a viewer that also has a
        // tools server advertises itself; self-reads would stall the UI
        // thread waiting on its own frame pump). Explicit attach_pid can
        // still target anything.
        if (t.pid == self) continue;
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

bool Client::dom_document(DomNodeInfo& out_root) {
    json::Value result;
    if (!impl_->wire_request("dom.document", {}, &result)) return false;
    const json::Value* root = result.get("root");
    if (root == nullptr || !root->is_object()) return false;
    node_from_json(*root, out_root);
    return true;
}

bool Client::dom_children(const DomHandle& nid,
                          std::vector<DomNodeInfo>& out) {
    out.clear();
    std::string params;
    nid_params(params, nid);
    json::Value result;
    if (!impl_->wire_request("dom.children", params, &result)) return false;
    const json::Value* nodes = result.get("nodes");
    if (nodes == nullptr) return false;
    out.reserve(nodes->array.size());
    for (const json::Value& n : nodes->array) {
        if (!n.is_object()) continue;
        out.emplace_back();
        node_from_json(n, out.back());
    }
    return true;
}

bool Client::dom_html(const DomHandle& nid, std::string& out_html) {
    std::string params;
    if (nid.node_slot != 0) nid_params(params, nid);
    json::Value result;
    if (!impl_->wire_request("dom.html", params, &result)) return false;
    out_html = std::string(result.get_string("html"));
    return true;
}

bool Client::css_box_model(const DomHandle& nid, BoxModel& out) {
    out = BoxModel{};
    std::string params;
    nid_params(params, nid);
    json::Value result;
    if (!impl_->wire_request("css.box_model", params, &result)) return false;
    const json::Value* laid = result.get("laid_out");
    out.laid_out =
        laid != nullptr && laid->kind == json::Value::Kind::Boolean &&
                   laid->boolean;
    if (!out.laid_out) return true;
    out.rect = rect_from_json(result.get("rect"));
    out.visual = rect_from_json(result.get("visual"));
    const json::Value* sides[3] = {result.get("margin"), result.get("border"),
                                   result.get("padding")};
    int* dests[3] = {out.margin, out.border, out.padding};
    for (int s = 0; s < 3; ++s) {
        if (sides[s] == nullptr || sides[s]->array.size() != 4) continue;
        for (int i = 0; i < 4; ++i) {
            dests[s][i] = static_cast<int>(sides[s]->array[
                static_cast<std::size_t>(i)].number);
        }
    }
    out.scroll_y = static_cast<int>(result.get_number("scroll_y"));
    out.content_h = static_cast<int>(result.get_number("content_h"));
    return true;
}

bool Client::stylesheets(std::vector<StylesheetInfo>& out) {
    out.clear();
    json::Value result;
    if (!impl_->wire_request("resource.stylesheets", {}, &result)) {
        return false;
    }
    const json::Value* sheets = result.get("sheets");
    if (sheets == nullptr) return false;
    out.reserve(sheets->array.size());
    for (const json::Value& s : sheets->array) {
        if (!s.is_object()) continue;
        StylesheetInfo info;
        info.index = static_cast<int>(s.get_number("index"));
        info.origin = std::string(s.get_string("origin"));
        info.label = std::string(s.get_string("label"));
        info.bytes = static_cast<long long>(s.get_number("bytes"));
        out.push_back(std::move(info));
    }
    return true;
}

bool Client::stylesheet_text(int index, std::string& out_text) {
    char params[48];
    std::snprintf(params, sizeof(params), "{\"index\":%d}", index);
    json::Value result;
    if (!impl_->wire_request("resource.stylesheet_text", params, &result)) {
        return false;
    }
    out_text = std::string(result.get_string("text"));
    return true;
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
