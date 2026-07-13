// Per-frame telemetry: JSONL formatting + the AFFINEUI_TELEMETRY file sink
// (data plane R1 — docs/TRACING_AND_PERFORMANCE_LOGGING.md, schema in
// docs/AFFINETOOLS_PROTOCOL.md). Assembly happens in the App frame
// callback (app.cpp); this file owns the record→text mapping and the sink.
//
// Conventions honored here (TRACING §1.7, §4):
//   • the sink gates on ONE relaxed atomic bool (sink_active()) — callers
//     branch on it so disabled paths never compute record contents;
//   • formatting is snprintf into caller buffers — no allocation;
//   • each line is flushed after write: JSONL survives a crash because
//     every written line is complete (a buffered record that dies with
//     the process is a record that never happened).

#include "affineui/telemetry.h"

#if AFFINEUI_PERF

#include "affineui/renderer.h"
#include "affineui/version.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <mutex>

#if defined(_WIN32)
    #include <process.h>
#else
    #include <unistd.h>
#endif

namespace affineui {

void fill_from_render_stats(FrameTelemetry& t, const RenderStats& rs) noexcept {
    t.prep_us        = rs.prepare_us_this_frame;
    t.layout_us      = rs.layout_us_this_frame;
    t.record_us      = rs.display_list_record_us_this_frame;
    t.raster_us      = rs.raster_us_this_frame;
    t.composite_us   = rs.composite_us_this_frame;
    t.cached_ops     = rs.cached_ops;
    t.culled_ops     = rs.display_list_ops_culled_this_frame;
    t.changed_ops    = rs.display_list_diff_changed_ops;
    t.dirty_rects    = rs.dirty_rects;
    t.dirty_pct_x100 = rs.dirty_area_pct_x100;
    t.recorded       = rs.recorded_this_frame;
    t.dl_changed     = rs.display_list_changed_this_frame;
    t.rasterized     = rs.root_layer_rasterized_this_frame;
    t.partial        = rs.root_layer_partial_this_frame;
    t.direct         = rs.root_layer_direct_this_frame;
    t.layer_reused   = rs.root_layer_reused_this_frame;
    t.animations     = rs.animations_active;
}

void fill_mem_delta(FrameTelemetry& t,
                    const mem::Stats& prev,
                    const mem::Stats& cur) noexcept {
    t.mem_live_bytes  = cur.live_bytes;
    t.mem_live_blocks = cur.live_blocks;
    t.allocs = static_cast<std::uint32_t>(cur.total_allocs - prev.total_allocs);
    t.frees  = static_cast<std::uint32_t>(cur.total_frees - prev.total_frees);
}

namespace {

const char* platform_name() noexcept {
#if defined(_WIN32)
    return "windows";
#elif defined(__APPLE__)
    return "macos";
#elif defined(__EMSCRIPTEN__)
    return "web";
#elif defined(__linux__)
    return "linux";
#else
    return "unknown";
#endif
}

int current_pid() noexcept {
#if defined(_WIN32)
    return _getpid();
#else
    return static_cast<int>(getpid());
#endif
}

std::size_t checked_len(int n, std::size_t cap) noexcept {
    // snprintf returns the would-be length; treat truncation as failure so
    // callers never emit a torn record.
    if (n < 0 || static_cast<std::size_t>(n) >= cap) return 0;
    return static_cast<std::size_t>(n);
}

}  // namespace

std::size_t format_session_jsonl(char* buf, std::size_t cap) {
    char t0_wall[32] = "unknown";
    const std::time_t now = std::time(nullptr);
    std::tm utc{};
#if defined(_WIN32)
    if (gmtime_s(&utc, &now) == 0)
#else
    if (gmtime_r(&now, &utc) != nullptr)
#endif
    {
        std::strftime(t0_wall, sizeof(t0_wall), "%Y-%m-%dT%H:%M:%SZ", &utc);
    }
    const int n = std::snprintf(
        buf, cap,
        "{\"v\":%u,\"type\":\"session\",\"schema\":\"telemetry/%u\","
        "\"affineui\":\"%s\",\"platform\":\"%s\",\"pid\":%d,"
        "\"t0_wall\":\"%s\"}",
        kTelemetrySchemaVersion, kTelemetrySchemaVersion,
        AFFINEUI_VERSION_STRING, platform_name(), current_pid(), t0_wall);
    return checked_len(n, cap);
}

std::size_t format_frame_jsonl(const FrameTelemetry& t,
                               char* buf, std::size_t cap) {
    const int n = std::snprintf(
        buf, cap,
        "{\"v\":%u,\"type\":\"frame\",\"frame\":%llu,\"t_ms\":%.2f,"
        "\"gap_ms\":%.2f,\"cb_ms\":%.2f,\"skipped\":%llu,"
        "\"fb\":[%d,%d],\"dpi\":%.2f,"
        "\"stage_us\":{\"prep\":%u,\"layout\":%u,\"dl\":%u,\"rast\":%u,"
        "\"comp\":%u},"
        "\"ops\":{\"cached\":%u,\"culled\":%u,\"changed\":%u,\"rects\":%u,"
        "\"dirty_pct\":%u.%02u},"
        "\"flags\":{\"rec\":%d,\"dl\":%d,\"rast\":%d,\"partial\":%d,"
        "\"direct\":%d,\"reused\":%d,\"anim\":%d},"
        "\"mem\":{\"live\":%llu,\"blocks\":%llu,\"allocs\":%u,\"frees\":%u}}",
        t.v,
        static_cast<unsigned long long>(t.frame),
        t.t_ms, t.gap_ms, t.cb_ms,
        static_cast<unsigned long long>(t.skipped),
        t.fb_w, t.fb_h, static_cast<double>(t.dpi),
        t.prep_us, t.layout_us, t.record_us, t.raster_us, t.composite_us,
        t.cached_ops, t.culled_ops, t.changed_ops, t.dirty_rects,
        t.dirty_pct_x100 / 100, t.dirty_pct_x100 % 100,
        t.recorded ? 1 : 0, t.dl_changed ? 1 : 0, t.rasterized ? 1 : 0,
        t.partial ? 1 : 0, t.direct ? 1 : 0, t.layer_reused ? 1 : 0,
        t.animations ? 1 : 0,
        static_cast<unsigned long long>(t.mem_live_bytes),
        static_cast<unsigned long long>(t.mem_live_blocks),
        t.allocs, t.frees);
    return checked_len(n, cap);
}

std::size_t format_idle_jsonl(double t_ms, std::uint64_t skipped,
                              char* buf, std::size_t cap) {
    const int n = std::snprintf(
        buf, cap,
        "{\"v\":%u,\"type\":\"idle\",\"t_ms\":%.2f,\"skipped\":%llu}",
        kTelemetrySchemaVersion, t_ms,
        static_cast<unsigned long long>(skipped));
    return checked_len(n, cap);
}

namespace telemetry {
namespace {

// Sink state. One per process (the env var is per-process); the atomic is
// the §1.7 fast gate, everything else is touched only under the mutex or
// during the one-time open.
std::atomic<bool> g_active{false};
FILE*             g_file{nullptr};
unsigned          g_every{1};
std::uint64_t     g_frame_records_seen{0};
std::once_flag    g_open_once;

std::mutex& sink_mutex() {
    static std::mutex m;
    return m;
}

void write_line(const char* buf, std::size_t len) {
    const std::lock_guard<std::mutex> lock(sink_mutex());
    if (!g_file) return;
    std::fwrite(buf, 1, len, g_file);
    std::fputc('\n', g_file);
    std::fflush(g_file);  // each line complete — survives a crash
}

void open_from_env_once() {
    const char* path = std::getenv("AFFINEUI_TELEMETRY");
    if (path == nullptr || path[0] == '\0') return;
#if defined(_WIN32)
    FILE* f = nullptr;
    if (fopen_s(&f, path, "w") != 0) f = nullptr;
#else
    FILE* f = std::fopen(path, "w");
#endif
    if (!f) {
        std::fprintf(stderr, "[telemetry] cannot open '%s'\n", path);
        std::fflush(stderr);
        return;
    }
    if (const char* every = std::getenv("AFFINEUI_TELEMETRY_EVERY");
        every != nullptr && every[0] != '\0') {
        const long n = std::strtol(every, nullptr, 10);
        if (n > 1) g_every = static_cast<unsigned>(n);
    }
    g_file = f;
    char buf[512];
    if (const std::size_t len = format_session_jsonl(buf, sizeof(buf));
        len > 0) {
        std::fwrite(buf, 1, len, g_file);
        std::fputc('\n', g_file);
        std::fflush(g_file);
    }
    std::atexit([] { sink_close(); });
    g_active.store(true, std::memory_order_release);
}

}  // namespace

bool sink_open_from_env() {
    std::call_once(g_open_once, open_from_env_once);
    return sink_active();
}

bool sink_active() noexcept {
    return g_active.load(std::memory_order_relaxed);
}

void sink_write_frame(const FrameTelemetry& t) {
    if (!sink_active()) return;
    if ((g_frame_records_seen++ % g_every) != 0) return;
    char buf[512];
    if (const std::size_t len = format_frame_jsonl(t, buf, sizeof(buf));
        len > 0) {
        write_line(buf, len);
    }
}

void sink_write_idle(double t_ms, std::uint64_t skipped) {
    if (!sink_active()) return;
    char buf[128];
    if (const std::size_t len =
            format_idle_jsonl(t_ms, skipped, buf, sizeof(buf));
        len > 0) {
        write_line(buf, len);
    }
}

void sink_close() {
    g_active.store(false, std::memory_order_release);
    const std::lock_guard<std::mutex> lock(sink_mutex());
    if (g_file) {
        std::fflush(g_file);
        std::fclose(g_file);
        g_file = nullptr;
    }
}

}  // namespace telemetry
}  // namespace affineui

#endif  // AFFINEUI_PERF
