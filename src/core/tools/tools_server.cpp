// affinetools protocol server (in-target half; AFFINETOOLS_DESIGN.md §2–3,
// wire schema docs/AFFINETOOLS_PROTOCOL.md).
//
// Thread model (DESIGN §2.1): ONE server thread owns the loopback socket,
// parses inbound JSON into typed handling, and formats/sends outbound
// events. The app thread only copies fixed-size records into a bounded
// ring under a copy-only critical section — it never blocks on the
// transport (sends happen on the server thread, which MAY block on a slow
// client; the ring then drops oldest and the drop count is surfaced as a
// telemetry.dropped event).
//
// Hard-to-crash (DESIGN §3.1): the 4-byte length prefix and payload are
// untrusted input inside the probed process. Max frame 1 MiB; zero-length,
// oversized, or malformed frames drop the connection — never an assert,
// never an unbounded allocation.

#include "affineui/tools.h"

#if AFFINEUI_PERF

#include "affineui/log.h"
#include "affineui/version.h"
#include "core/log_internal.h"
#include "core/tools/tools_commands.h"
// Inbound wire messages are parsed with tiny-json (external/tinyjson) —
// a ~600 LOC single-header C parser with zero heap allocations. Same
// code path in modular + amalgamated builds; the parser vendors cleanly
// into the two-file SDK.
extern "C" {
#include "tiny-json.h"
}

#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <utility>
#include <vector>

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
static constexpr socket_t kInvalidSocket = INVALID_SOCKET;
#else
    #include <arpa/inet.h>
    #include <netinet/in.h>
    #include <sys/select.h>
    #include <sys/socket.h>
    #include <sys/stat.h>
    #include <unistd.h>
#if defined(__APPLE__)
    #include <mach-o/dyld.h>   // _NSGetExecutablePath
    #include <sys/syslimits.h> // PATH_MAX
#endif
using socket_t = int;
static constexpr socket_t kInvalidSocket = -1;
#endif

#if defined(_WIN32) && defined(small)
// Legacy RPC token from the Windows headers. In the amalgamated build it
// would otherwise rewrite later C++ members such as `opts.small`.
    #undef small
#endif

namespace affineui {
namespace {

constexpr std::uint32_t kMaxInboundFrame = 1024u * 1024u;  // 1 MiB
constexpr int           kProtocolVersion = 0;
constexpr std::size_t   kRingCapacity    = 256;

void close_socket(socket_t s) noexcept {
    if (s == kInvalidSocket) return;
#if defined(_WIN32)
    closesocket(s);
#else
    close(s);
#endif
}

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

int tools_pid() noexcept {
#if defined(_WIN32)
    return _getpid();
#else
    return static_cast<int>(getpid());
#endif
}

std::string random_hex(std::size_t bytes) {
    std::random_device rd;
    std::string out;
    out.reserve(bytes * 2);
    static const char* hex = "0123456789abcdef";
    for (std::size_t i = 0; i < bytes; ++i) {
        const unsigned v = rd() & 0xFFu;
        out.push_back(hex[v >> 4]);
        out.push_back(hex[v & 0xFu]);
    }
    return out;
}

void json_escape_into(std::string& out, std::string_view s) {
    for (const char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (static_cast<unsigned char>(c) < 0x20u) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x",
                                  static_cast<unsigned>(c));
                    out += buf;
                } else {
                    out.push_back(c);
                }
        }
    }
}

std::string utc_now_iso() {
    char buf[32] = "unknown";
    const std::time_t now = std::time(nullptr);
    std::tm utc{};
#if defined(_WIN32)
    if (gmtime_s(&utc, &now) == 0)
#else
    if (gmtime_r(&now, &utc) != nullptr)
#endif
    {
        std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &utc);
    }
    return buf;
}

std::string executable_path() {
#if defined(_WIN32)
    char buf[512];
    const DWORD n = GetModuleFileNameA(nullptr, buf, sizeof(buf));
    return (n > 0 && n < sizeof(buf)) ? std::string(buf, n) : std::string{};
#elif defined(__linux__)
    char buf[512];
    const ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    return n > 0 ? std::string(buf, static_cast<std::size_t>(n))
                 : std::string{};
#elif defined(__APPLE__)
    // _NSGetExecutablePath fills the buffer with the invocation path,
    // which may contain `..` / symlinks — realpath resolves it so
    // parent_dir() walks give a canonical directory tree that
    // find_devtools_exe() can compare against sibling `affinetools`
    // and `tools/affinetools/affinetools` markers.
    char raw[PATH_MAX];
    std::uint32_t sz = sizeof(raw);
    if (_NSGetExecutablePath(raw, &sz) != 0) return {};
    char resolved[PATH_MAX];
    if (realpath(raw, resolved) != nullptr) return std::string(resolved);
    return std::string(raw);
#else
    return {};
#endif
}

std::string discovery_dir() {
#if defined(_WIN32)
    char buf[512];
    const DWORD n = GetTempPathA(sizeof(buf), buf);
    std::string dir = (n > 0 && n < sizeof(buf)) ? std::string(buf, n)
                                                 : std::string{"."};
    if (!dir.empty() && dir.back() != '\\' && dir.back() != '/') dir += '\\';
    return dir + "affineui-tools";
#else
    const char* tmp = std::getenv("TMPDIR");
    std::string dir = (tmp != nullptr && tmp[0] != '\0') ? tmp : "/tmp";
    if (!dir.empty() && dir.back() != '/') dir += '/';
    return dir + "affineui-tools";
#endif
}

void make_dir(const std::string& path) {
#if defined(_WIN32)
    CreateDirectoryA(path.c_str(), nullptr);
#else
    mkdir(path.c_str(), 0700);
#endif
}

// ── queued records (app thread → server thread) ─────────────────────────

struct QueuedRecord {
    bool           idle{false};
    FrameTelemetry t{};  // idle records use t.t_ms / t.skipped only
};

// A diagnostic line queued for the log domain. The budget (kLogRingCapacity)
// caps memory when a client is slow or absent; drop-oldest, like telemetry.
struct QueuedLog {
    int           level{1};       // 0 debug, 1 info, 2 warn, 3 error
    std::uint64_t frame{0};       // presented-frame index at emission
    double        t_ms{0.0};
    std::string   msg;
};
constexpr std::size_t kLogRingCapacity = 1024;  // log budget (lines)

struct ServerState {
    std::atomic<bool> running{false};
    std::atomic<bool> subscribed{false};  // authed client wants telemetry
    std::atomic<bool> log_subscribed{false};  // authed client wants logs

    std::thread thread;
    socket_t    listen_sock{kInvalidSocket};
    int         port{0};
    std::string token;
    std::string session_id;
    std::string discovery_file;

    // Bounded record ring. Critical sections are copy-only (a few hundred
    // bytes) — the app thread never waits on socket IO through this lock.
    std::mutex                              ring_mutex;
    std::array<QueuedRecord, kRingCapacity> ring{};
    std::size_t                             ring_head{0};
    std::size_t                             ring_count{0};
    std::uint64_t                           ring_dropped{0};

    // Log ring (budgeted, drop-oldest). Separate mutex so a burst of log
    // lines never contends with the telemetry ring.
    std::mutex               log_mutex;
    std::deque<QueuedLog>    log_ring;
    std::uint64_t            log_dropped{0};

    // Read-command bridge (DESIGN §2.1): server thread enqueues typed
    // commands (bounded — overflow answers "busy" immediately); the app
    // thread drains them in tools_pump and pushes complete response
    // documents back for the server thread to frame and send.
    std::mutex                             cmd_mutex;
    std::deque<detail::ToolsCommand>       cmd_queue;
    std::atomic<int>                       cmd_pending{0};
    std::mutex                             resp_mutex;
    std::deque<std::pair<std::uint64_t, std::string>> resp_queue;  // {epoch, json}
    // Connection epoch: advanced by drop_connection (server thread only).
    // Commands are stamped with it at enqueue; a response whose epoch no
    // longer matches belongs to a client that is gone and is discarded.
    // Closes the race where the app thread holds taken commands across a
    // disconnect and pushes their responses after the queues were cleared
    // — without the stamp those would leak into the NEXT client's stream,
    // whose request ids start over and would collide.
    std::atomic<std::uint64_t>             conn_epoch{0};
};
constexpr std::size_t kCommandQueueCapacity = 32;

ServerState& state() {
    static ServerState s;
    return s;
}

// ── framing ──────────────────────────────────────────────────────────────

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

// ── per-connection handling (server thread only) ─────────────────────────

struct Connection {
    socket_t          sock{kInvalidSocket};
    bool              authed{false};
    std::vector<char> inbuf;
};

void send_error(Connection& c, long long id, int code, const char* message) {
    char buf[256];
    const int n = std::snprintf(
        buf, sizeof(buf),
        "{\"id\":%lld,\"error\":{\"code\":%d,\"message\":\"%s\"}}", id, code,
        message);
    if (n > 0 && static_cast<std::size_t>(n) < sizeof(buf)) {
        (void) send_framed(c.sock, std::string_view(buf, static_cast<std::size_t>(n)));
    }
}

void send_result(Connection& c, long long id, std::string_view result_json) {
    std::string msg;
    msg.reserve(result_json.size() + 32);
    char head[32];
    std::snprintf(head, sizeof(head), "{\"id\":%lld,\"result\":", id);
    msg += head;
    msg += result_json;
    msg += '}';
    (void) send_framed(c.sock, msg);
}

// Small helpers around tiny-json's C surface. tiny-json's json_t union
// stores every non-container leaf as a null-terminated char*, so string
// access just wraps the pointer; number access runs strtod once.
namespace {

std::string_view tj_str(json_t const* obj, const char* prop) noexcept {
    if (!obj) return {};
    json_t const* v = json_getProperty(obj, prop);
    if (!v || v->type != JSON_TEXT || !v->u.value) return {};
    return std::string_view{v->u.value};
}

double tj_num(json_t const* obj, const char* prop, double fallback) noexcept {
    if (!obj) return fallback;
    json_t const* v = json_getProperty(obj, prop);
    if (!v || !v->u.value) return fallback;
    if (v->type != JSON_INTEGER && v->type != JSON_REAL) return fallback;
    return std::strtod(v->u.value, nullptr);
}

// Ceiling on tokens per inbound message — hello / subscribe / etc. are
// tiny (top-level object + maybe one params sub-object with a couple of
// leaves). 64 is roomy for anything the current protocol emits and still
// fits comfortably on the stack.
constexpr unsigned kJsonPoolSize = 64;

/// Parse a `"nid":[document_id,node_slot,generation]` param. Returns an
/// empty (falsy) handle when absent/malformed — commands that require a
/// node answer a "stale node" error from the pump.
affineui::DomHandle tj_nid(json_t const* params) noexcept {
    affineui::DomHandle out{};
    if (!params) return out;
    json_t const* arr = json_getProperty(params, "nid");
    if (!arr || json_getType(arr) != JSON_ARRAY) return out;
    std::uint32_t vals[3] = {0, 0, 0};
    int i = 0;
    for (json_t const* item = json_getChild(arr); item != nullptr && i < 3;
         item = json_getSibling(item), ++i) {
        if (json_getType(item) != JSON_INTEGER || !item->u.value) return {};
        vals[i] = static_cast<std::uint32_t>(
            std::strtoul(item->u.value, nullptr, 10));
    }
    if (i != 3) return {};
    out.document_id = vals[0];
    out.node_slot = vals[1];
    out.generation = vals[2];
    return out;
}

}  // namespace

/// Returns false when the connection must be dropped.
bool handle_message(ServerState& st, Connection& c, std::string_view text) {
    // tiny-json parses in place (writes '\0' terminators into the buffer),
    // so hand it a mutable copy. Inbound frames are already size-bounded
    // by drain_inbuf, so a std::string is fine here.
    std::string buf(text);
    json_t pool[kJsonPoolSize];
    json_t const* msg = json_create(buf.data(), pool, kJsonPoolSize);
    if (!msg || json_getType(msg) != JSON_OBJ) {
        return false;  // malformed → disconnect (hard-to-crash rule)
    }

    const std::string_view method = tj_str(msg, "method");
    const long long id = static_cast<long long>(tj_num(msg, "id", -1.0));

    if (!c.authed) {
        // Only hello is accepted before auth; a bad token disconnects.
        if (method != "hello") return false;
        json_t const* params = json_getProperty(msg, "params");
        const std::string_view token = tj_str(params, "token");
        if (token.empty() || token != st.token) return false;
        c.authed = true;
        std::string result;
        result.reserve(256);
        result += "{\"protocol\":";
        char num[16];
        std::snprintf(num, sizeof(num), "%d", kProtocolVersion);
        result += num;
        result += ",\"affineui\":\"" AFFINEUI_VERSION_STRING "\"";
        result += ",\"session_id\":\"";
        result += st.session_id;  // hex — no escaping needed
        result += "\",\"capabilities\":[\"telemetry\",\"log\",\"dom\","
                  "\"css\",\"resource\"],\"t0_wall\":\"";
        result += utc_now_iso();
        result += "\"}";
        send_result(c, id, result);
        return true;
    }

    if (method == "ping") {
        send_result(c, id, "{}");
        return true;
    }
    if (method == "telemetry.subscribe") {
        st.subscribed.store(true, std::memory_order_release);
        send_result(c, id, "{}");
        return true;
    }
    if (method == "telemetry.unsubscribe") {
        st.subscribed.store(false, std::memory_order_release);
        send_result(c, id, "{}");
        return true;
    }
    if (method == "log.subscribe") {
        st.log_subscribed.store(true, std::memory_order_release);
        send_result(c, id, "{}");
        return true;
    }
    if (method == "log.unsubscribe") {
        st.log_subscribed.store(false, std::memory_order_release);
        send_result(c, id, "{}");
        return true;
    }

    // Read commands (dom/css/resource): parse into a typed command and
    // queue for the app thread; the response is produced by tools_pump
    // and sent from flush_responses. Bounded queue — overflow answers
    // "busy" immediately rather than growing with client eagerness.
    using detail::ToolsCommand;
    ToolsCommand cmd;
    cmd.id = id;
    bool is_read_command = true;
    json_t const* params = json_getProperty(msg, "params");
    if (method == "dom.document") {
        cmd.kind = ToolsCommand::Kind::DomDocument;
    } else if (method == "dom.children") {
        cmd.kind = ToolsCommand::Kind::DomChildren;
        cmd.nid = tj_nid(params);
    } else if (method == "dom.html") {
        cmd.kind = ToolsCommand::Kind::DomHtml;
        cmd.nid = tj_nid(params);  // empty handle = whole document
    } else if (method == "css.box_model") {
        cmd.kind = ToolsCommand::Kind::CssBoxModel;
        cmd.nid = tj_nid(params);
    } else if (method == "resource.stylesheets") {
        cmd.kind = ToolsCommand::Kind::ResourceStylesheets;
    } else if (method == "resource.stylesheet_text") {
        cmd.kind = ToolsCommand::Kind::ResourceStylesheetText;
        cmd.index = static_cast<int>(tj_num(params, "index", -1.0));
    } else {
        is_read_command = false;
    }
    if (is_read_command) {
        cmd.epoch = st.conn_epoch.load(std::memory_order_relaxed);
        bool queued = false;
        {
            const std::lock_guard<std::mutex> lock(st.cmd_mutex);
            if (st.cmd_queue.size() < kCommandQueueCapacity) {
                st.cmd_queue.push_back(cmd);
                queued = true;
            }
        }
        if (queued) {
            st.cmd_pending.fetch_add(1, std::memory_order_release);
        } else {
            send_error(c, id, -32005, "busy");
        }
        return true;
    }

    send_error(c, id, -32601, "unknown method");
    return true;
}

/// Consume complete frames from the connection buffer. Returns false when
/// the connection must be dropped.
bool drain_inbuf(ServerState& st, Connection& c) {
    std::size_t off = 0;
    while (c.inbuf.size() - off >= 4) {
        const unsigned char* p =
            reinterpret_cast<const unsigned char*>(c.inbuf.data() + off);
        const std::uint32_t len =
            static_cast<std::uint32_t>(p[0]) |
            (static_cast<std::uint32_t>(p[1]) << 8) |
            (static_cast<std::uint32_t>(p[2]) << 16) |
            (static_cast<std::uint32_t>(p[3]) << 24);
        if (len == 0 || len > kMaxInboundFrame) return false;
        if (c.inbuf.size() - off - 4 < len) break;  // incomplete frame
        const std::string_view payload(c.inbuf.data() + off + 4, len);
        if (!handle_message(st, c, payload)) return false;
        off += 4 + len;
    }
    if (off > 0) {
        c.inbuf.erase(c.inbuf.begin(),
                      c.inbuf.begin() + static_cast<std::ptrdiff_t>(off));
    }
    // A client that streams garbage without ever completing a frame is
    // bounded by kMaxInboundFrame via the length check above; cap the
    // buffer to frame size + header for safety.
    return c.inbuf.size() <= kMaxInboundFrame + 4;
}

void drop_connection(ServerState& st, Connection& c);

void flush_ring(ServerState& st, Connection& c) {
    if (c.sock == kInvalidSocket || !c.authed ||
        !st.subscribed.load(std::memory_order_acquire)) {
        // Nobody to send to: clear the ring so records don't go stale.
        const std::lock_guard<std::mutex> lock(st.ring_mutex);
        st.ring_head = 0;
        st.ring_count = 0;
        st.ring_dropped = 0;
        return;
    }
    std::vector<QueuedRecord> batch;
    std::uint64_t dropped = 0;
    {
        const std::lock_guard<std::mutex> lock(st.ring_mutex);
        batch.reserve(st.ring_count);
        for (std::size_t i = 0; i < st.ring_count; ++i) {
            batch.push_back(st.ring[(st.ring_head + i) % kRingCapacity]);
        }
        st.ring_head = 0;
        st.ring_count = 0;
        dropped = st.ring_dropped;
        st.ring_dropped = 0;
    }
    if (dropped > 0) {
        char buf[128];
        const int n = std::snprintf(
            buf, sizeof(buf),
            "{\"method\":\"telemetry.dropped\",\"params\":{\"count\":%llu}}",
            static_cast<unsigned long long>(dropped));
        if (n > 0) {
            if (!send_framed(c.sock,
                             std::string_view(buf, static_cast<std::size_t>(n)))) {
                drop_connection(st, c);
                return;
            }
        }
    }
    for (const QueuedRecord& rec : batch) {
        char record[512];
        std::size_t len;
        const char* method;
        if (rec.idle) {
            len = format_idle_jsonl(rec.t.t_ms, rec.t.skipped, record,
                                    sizeof(record));
            method = "target.idle";
        } else {
            len = format_frame_jsonl(rec.t, record, sizeof(record));
            method = "telemetry.frame";
        }
        if (len == 0) continue;
        std::string msg;
        msg.reserve(len + 48);
        msg += "{\"method\":\"";
        msg += method;
        msg += "\",\"params\":";
        msg.append(record, len);
        msg += '}';
        if (!send_framed(c.sock, msg)) {
            drop_connection(st, c);
            return;
        }
    }
}

const char* log_level_name(int level) {
    switch (level) {
        case 0: return "debug";
        case 2: return "warn";
        case 3: return "error";
        default: return "info";
    }
}

void flush_log_ring(ServerState& st, Connection& c) {
    if (c.sock == kInvalidSocket || !c.authed ||
        !st.log_subscribed.load(std::memory_order_acquire)) {
        // Not streaming logs: keep the ring bounded but don't clear it —
        // a client may subscribe later and want recent history. (The ring
        // is already capped at kLogRingCapacity by push_log.)
        return;
    }
    std::deque<QueuedLog> batch;
    std::uint64_t dropped = 0;
    {
        const std::lock_guard<std::mutex> lock(st.log_mutex);
        batch.swap(st.log_ring);
        dropped = st.log_dropped;
        st.log_dropped = 0;
    }
    if (dropped > 0) {
        char buf[128];
        const int n = std::snprintf(
            buf, sizeof(buf),
            "{\"method\":\"log.dropped\",\"params\":{\"count\":%llu}}",
            static_cast<unsigned long long>(dropped));
        if (n > 0 && !send_framed(c.sock, std::string_view(
                                              buf, static_cast<std::size_t>(n)))) {
            drop_connection(st, c);
            return;
        }
    }
    for (const QueuedLog& e : batch) {
        std::string msg;
        msg.reserve(e.msg.size() + 96);
        char head[96];
        std::snprintf(head, sizeof(head),
                      "{\"method\":\"log.line\",\"params\":{\"level\":\"%s\","
                      "\"frame\":%llu,\"t_ms\":%.2f,\"text\":\"",
                      log_level_name(e.level),
                      static_cast<unsigned long long>(e.frame), e.t_ms);
        msg += head;
        json_escape_into(msg, e.msg);
        msg += "\"}}";
        if (!send_framed(c.sock, msg)) {
            drop_connection(st, c);
            return;
        }
    }
}

/// Drain app-thread responses to the client. With no (authed) client the
/// responses are discarded — they belong to a connection that is gone,
/// and the one-client policy means the next client must never receive
/// them.
void flush_responses(ServerState& st, Connection& c) {
    std::deque<std::pair<std::uint64_t, std::string>> batch;
    {
        const std::lock_guard<std::mutex> lock(st.resp_mutex);
        batch.swap(st.resp_queue);
    }
    if (batch.empty()) return;
    if (c.sock == kInvalidSocket || !c.authed) return;  // discard
    const std::uint64_t epoch =
        st.conn_epoch.load(std::memory_order_relaxed);
    for (const auto& [resp_epoch, msg] : batch) {
        if (resp_epoch != epoch) continue;  // a gone client's response
        if (!send_framed(c.sock, msg)) {
            drop_connection(st, c);
            return;
        }
    }
}

void drop_connection(ServerState& st, Connection& c) {
    if (c.sock != kInvalidSocket) {
        close_socket(c.sock);
        c.sock = kInvalidSocket;
    }
    c.authed = false;
    c.inbuf.clear();
    st.subscribed.store(false, std::memory_order_release);
    st.log_subscribed.store(false, std::memory_order_release);
    // Commands not yet serviced belong to the dropped client; advance the
    // epoch FIRST so a response the app thread pushes concurrently (from
    // commands it already took) is stamped stale, then clear both queues
    // so the next client never sees them.
    st.conn_epoch.fetch_add(1, std::memory_order_relaxed);
    {
        const std::lock_guard<std::mutex> lock(st.cmd_mutex);
        if (!st.cmd_queue.empty()) {
            st.cmd_pending.fetch_sub(static_cast<int>(st.cmd_queue.size()),
                                     std::memory_order_release);
            st.cmd_queue.clear();
        }
    }
    {
        const std::lock_guard<std::mutex> lock(st.resp_mutex);
        st.resp_queue.clear();
    }
}

void server_loop(ServerState& st) {
    Connection conn;
    while (st.running.load(std::memory_order_acquire)) {
        fd_set readable;
        FD_ZERO(&readable);
        socket_t maxfd = st.listen_sock;
        FD_SET(st.listen_sock, &readable);
        if (conn.sock != kInvalidSocket) {
            FD_SET(conn.sock, &readable);
            if (conn.sock > maxfd) maxfd = conn.sock;
        }
        timeval tv{};
        tv.tv_usec = 50 * 1000;  // 50 ms tick: bounded event latency
        const int rc = select(static_cast<int>(maxfd + 1), &readable, nullptr,
                              nullptr, &tv);
        if (!st.running.load(std::memory_order_acquire)) break;
        if (rc > 0) {
            if (FD_ISSET(st.listen_sock, &readable)) {
                const socket_t incoming = accept(st.listen_sock, nullptr, nullptr);
                if (incoming != kInvalidSocket) {
                    if (conn.sock == kInvalidSocket) {
                        // Bound sends so a frozen client can't wedge the
                        // server thread (and with it, shutdown/join).
#if defined(_WIN32)
                        const DWORD send_timeout_ms = 2000;
                        setsockopt(incoming, SOL_SOCKET, SO_SNDTIMEO,
                                   reinterpret_cast<const char*>(&send_timeout_ms),
                                   sizeof(send_timeout_ms));
#else
                        timeval send_timeout{2, 0};
                        setsockopt(incoming, SOL_SOCKET, SO_SNDTIMEO,
                                   &send_timeout, sizeof(send_timeout));
#endif
                        conn.sock = incoming;
                    } else {
                        close_socket(incoming);  // one client at a time
                    }
                }
            }
            if (conn.sock != kInvalidSocket && FD_ISSET(conn.sock, &readable)) {
                char buf[4096];
                const int n = recv(conn.sock, buf, sizeof(buf), 0);
                if (n <= 0) {
                    drop_connection(st, conn);
                } else {
                    conn.inbuf.insert(conn.inbuf.end(), buf, buf + n);
                    if (!drain_inbuf(st, conn)) drop_connection(st, conn);
                }
            }
        }
        flush_ring(st, conn);
        flush_log_ring(st, conn);
        flush_responses(st, conn);
    }
    drop_connection(st, conn);
}

void write_discovery_file(ServerState& st) {
    const std::string dir = discovery_dir();
    make_dir(dir);
    char name[64];
    std::snprintf(name, sizeof(name), "%d.json", tools_pid());
#if defined(_WIN32)
    st.discovery_file = dir + '\\' + name;
#else
    st.discovery_file = dir + '/' + name;
#endif
    FILE* f = nullptr;
#if defined(_WIN32)
    if (fopen_s(&f, st.discovery_file.c_str(), "w") != 0) f = nullptr;
#else
    f = std::fopen(st.discovery_file.c_str(), "w");
#endif
    if (!f) {
        std::fprintf(stderr, "[tools] cannot write discovery file '%s'\n",
                     st.discovery_file.c_str());
        std::fflush(stderr);
        st.discovery_file.clear();
        return;
    }
    std::string exe;
    json_escape_into(exe, executable_path());
    std::fprintf(f,
                 "{\"pid\":%d,\"port\":%d,\"token\":\"%s\","
                 "\"exe\":\"%s\",\"affineui\":\"%s\"}\n",
                 tools_pid(), st.port, st.token.c_str(), exe.c_str(),
                 AFFINEUI_VERSION_STRING);
    std::fclose(f);
}

}  // namespace

// Log forwarder (defined below) registered with the core log facility.
void forward_log(const LogRecord& record);

// ── public API ────────────────────────────────────────────────────────────

bool tools_listen(int port) {
    ServerState& st = state();
    if (st.running.load(std::memory_order_acquire)) return true;
    if (!sockets_init()) return false;

    socket_t s = socket(AF_INET, SOCK_STREAM, 0);
    if (s == kInvalidSocket) return false;

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);  // never a public bind
    addr.sin_port = htons(static_cast<std::uint16_t>(port));
    if (bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 ||
        listen(s, 1) != 0) {
        close_socket(s);
        return false;
    }
    sockaddr_in bound{};
#if defined(_WIN32)
    int blen = sizeof(bound);
#else
    socklen_t blen = sizeof(bound);
#endif
    if (getsockname(s, reinterpret_cast<sockaddr*>(&bound), &blen) != 0) {
        close_socket(s);
        return false;
    }

    st.listen_sock = s;
    st.port = static_cast<int>(ntohs(bound.sin_port));
    st.token = random_hex(16);
    st.session_id = random_hex(8);
    write_discovery_file(st);
    st.running.store(true, std::memory_order_release);
    st.thread = std::thread([&st] { server_loop(st); });
    // Tap the core log facility so AffineUI diagnostics reach the panel.
    detail::set_tools_log_forwarder(&forward_log);

    static std::once_flag exit_hook;
    std::call_once(exit_hook, [] { std::atexit([] { tools_shutdown(); }); });

    std::fprintf(stderr, "[tools] listening on 127.0.0.1:%d (pid %d)\n",
                 st.port, tools_pid());
    std::fflush(stderr);
    return true;
}

bool tools_listen_from_env() {
    const char* e = std::getenv("AFFINEUI_TOOLS_LISTEN");
    if (e == nullptr || e[0] == '\0' || (e[0] == '0' && e[1] == '\0')) {
        return false;
    }
    const long port = std::strtol(e, nullptr, 10);
    return tools_listen(port > 1 ? static_cast<int>(port) : 0);
}

bool tools_active() noexcept {
    return state().running.load(std::memory_order_relaxed);
}

namespace {

bool file_exists(const std::string& path) noexcept {
#if defined(_WIN32)
    const DWORD attrs = GetFileAttributesA(path.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES &&
           (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0;
#else
    return access(path.c_str(), X_OK) == 0;
#endif
}

std::string parent_dir(const std::string& path) {
    const std::size_t slash = path.find_last_of("/\\");
    return slash == std::string::npos ? std::string{} : path.substr(0, slash);
}

/// Zero-config viewer lookup: env override → beside the exe → the build
/// tree walking up from the exe dir (tools/affinetools/) → empty (caller
/// falls back to a PATH launch).
std::string find_devtools_exe() {
#if defined(_WIN32)
    const char* kName = "affinetools.exe";
#else
    const char* kName = "affinetools";
#endif
    if (const char* env = std::getenv("AFFINEUI_TOOLS_EXE");
        env != nullptr && env[0] != '\0' && file_exists(env)) {
        return env;
    }
    std::string dir = parent_dir(executable_path());
    for (int depth = 0; depth < 8 && !dir.empty(); ++depth) {
        const std::string sibling = dir + "/" + kName;
        if (file_exists(sibling)) return sibling;
        const std::string built = dir + "/tools/affinetools/" + kName;
        if (file_exists(built)) return built;
        dir = parent_dir(dir);
    }
    return {};
}

bool spawn_detached(const std::string& exe_or_name, int target_pid) {
#if defined(_WIN32)
    char cmd[1024];
    std::snprintf(cmd, sizeof(cmd), "\"%s\" --pid %d", exe_or_name.c_str(),
                  target_pid);
    STARTUPINFOA si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    // lpApplicationName null → the loader resolves the (quoted) first
    // token, searching PATH when it's a bare name.
    if (!CreateProcessA(nullptr, cmd, nullptr, nullptr, FALSE,
                        CREATE_NEW_PROCESS_GROUP, nullptr, nullptr, &si,
                        &pi)) {
        return false;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
#else
    char pid_arg[16];
    std::snprintf(pid_arg, sizeof(pid_arg), "%d", target_pid);
    const pid_t child = fork();
    if (child < 0) return false;
    if (child == 0) {
        execlp(exe_or_name.c_str(), exe_or_name.c_str(), "--pid", pid_arg,
               static_cast<char*>(nullptr));
        _exit(127);
    }
    return true;
#endif
}

}  // namespace

bool tools_open_devtools() {
    // Debounce: F12 auto-repeat / double-taps shouldn't spawn a fleet of
    // viewers (the server only accepts one client anyway).
    static std::atomic<long long> last_open_ms{-1000000};
    const long long now_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count();
    const long long prev = last_open_ms.exchange(now_ms);
    if (now_ms - prev < 2000) return true;

    if (!tools_active() && !tools_listen(0)) {
        std::fprintf(stderr, "[tools] devtools: server failed to start\n");
        std::fflush(stderr);
        return false;
    }
    std::string exe = find_devtools_exe();
    if (exe.empty()) {
#if defined(_WIN32)
        exe = "affinetools.exe";  // last resort: PATH
#else
        exe = "affinetools";
#endif
    }
    if (!spawn_detached(exe, tools_pid())) {
        std::fprintf(stderr,
                     "[tools] devtools viewer not found (looked beside the "
                     "exe, in the build tree, and on PATH; set "
                     "AFFINEUI_TOOLS_EXE to override)\n");
        std::fflush(stderr);
        return false;
    }
    std::fprintf(stderr, "[tools] devtools opening: %s --pid %d\n",
                 exe.c_str(), tools_pid());
    std::fflush(stderr);
    return true;
}

int tools_port() noexcept {
    return tools_active() ? state().port : 0;
}

void tools_shutdown() {
    ServerState& st = state();
    if (!st.running.exchange(false, std::memory_order_acq_rel)) return;
    detail::set_tools_log_forwarder(nullptr);  // stop tapping the log facility
    st.subscribed.store(false, std::memory_order_release);
    st.log_subscribed.store(false, std::memory_order_release);
    close_socket(st.listen_sock);  // also wakes select via readability
    if (st.thread.joinable()) st.thread.join();
    st.listen_sock = kInvalidSocket;
    if (!st.discovery_file.empty()) {
        std::remove(st.discovery_file.c_str());
        st.discovery_file.clear();
    }
    st.port = 0;
}

// Log forwarder registered with the core log facility. Runs on whatever
// thread emitted the log (usually the app thread). Copy-only critical
// section; drop-oldest past the budget. Never blocks on IO — the server
// thread drains and sends.
void forward_log(const LogRecord& record) {
    ServerState& st = state();
    if (!st.running.load(std::memory_order_relaxed)) return;
    // Even with no subscriber we keep a bounded recent-history ring so a
    // client that subscribes mid-session sees the latest lines.
    QueuedLog e;
    switch (record.level) {
        case LogLevel::debug: e.level = 0; break;
        case LogLevel::warn:  e.level = 2; break;
        case LogLevel::error: e.level = 3; break;
        default:              e.level = 1; break;
    }
    e.frame = record.frame;
    e.t_ms = record.t_ms;
    e.msg.assign(record.msg.data(), record.msg.size());
    const std::lock_guard<std::mutex> lock(st.log_mutex);
    if (st.log_ring.size() >= kLogRingCapacity) {
        st.log_ring.pop_front();
        ++st.log_dropped;
    }
    st.log_ring.push_back(std::move(e));
}

// ── command bridge (tools_commands.h) ────────────────────────────────────

namespace detail {

bool tools_take_commands(std::vector<ToolsCommand>& out) {
    ServerState& st = state();
    const std::lock_guard<std::mutex> lock(st.cmd_mutex);
    if (st.cmd_queue.empty()) return false;
    out.insert(out.end(), st.cmd_queue.begin(), st.cmd_queue.end());
    st.cmd_pending.fetch_sub(static_cast<int>(st.cmd_queue.size()),
                             std::memory_order_release);
    st.cmd_queue.clear();
    return true;
}

void tools_push_response(std::uint64_t epoch, std::string json) {
    ServerState& st = state();
    const std::lock_guard<std::mutex> lock(st.resp_mutex);
    // Bounded by the command queue: responses only exist for commands
    // that were accepted (≤ kCommandQueueCapacity in flight). The epoch
    // travels with the response; flush_responses discards it when the
    // originating connection is gone.
    st.resp_queue.emplace_back(epoch, std::move(json));
}

bool tools_has_pending_commands() noexcept {
    return state().cmd_pending.load(std::memory_order_relaxed) > 0;
}

void tools_json_escape(std::string& out, std::string_view s) {
    json_escape_into(out, s);
}

}  // namespace detail

bool tools_has_pending() noexcept {
    return detail::tools_has_pending_commands();
}

namespace tools {

bool wants_telemetry() noexcept {
    return state().subscribed.load(std::memory_order_relaxed);
}

void push_frame(const FrameTelemetry& t) {
    ServerState& st = state();
    const std::lock_guard<std::mutex> lock(st.ring_mutex);
    if (st.ring_count == kRingCapacity) {
        st.ring_head = (st.ring_head + 1) % kRingCapacity;
        --st.ring_count;
        ++st.ring_dropped;
    }
    QueuedRecord& slot =
        st.ring[(st.ring_head + st.ring_count) % kRingCapacity];
    slot.idle = false;
    slot.t = t;
    ++st.ring_count;
}

void push_idle(double t_ms, std::uint64_t skipped) {
    ServerState& st = state();
    const std::lock_guard<std::mutex> lock(st.ring_mutex);
    if (st.ring_count == kRingCapacity) {
        st.ring_head = (st.ring_head + 1) % kRingCapacity;
        --st.ring_count;
        ++st.ring_dropped;
    }
    QueuedRecord& slot =
        st.ring[(st.ring_head + st.ring_count) % kRingCapacity];
    slot.idle = true;
    slot.t = FrameTelemetry{};
    slot.t.t_ms = t_ms;
    slot.t.skipped = skipped;
    ++st.ring_count;
}

}  // namespace tools
}  // namespace affineui

#endif  // AFFINEUI_PERF
