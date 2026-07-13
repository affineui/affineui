// AffineUI diagnostic logging facility (affineui/log.h).
//
// One funnel: detail::log_msg (embed_log.h, the historical entry point)
// now builds a LogRecord — stamping the current presented-frame index via
// a frame-source hook the App installs — and dispatches it to the active
// handler. The default handler fans out to the console and, when an
// affinetools viewer is attached, to its log ring. A host can replace the
// handler; the two default sinks are exposed so a custom handler can reuse
// either.
//
// The legacy raw-pointer LogFn sink (InitDesc::log) still fires in addition
// to the handler, for embedders that installed one.

#include "affineui/log.h"

#include "core/embed_log.h"
#include "core/log_internal.h"

#include <atomic>
#include <cstdio>
#include <mutex>
#include <utility>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
    #include <windows.h>
#endif

namespace affineui {

// The FrameSource / ToolsLogForwarder aliases + setters come from
// internal/log_internal.h; legacy_log_fn/user from internal/embed_log.h.

namespace {

std::mutex&              handler_mutex() { static std::mutex m; return m; }
LogHandler&              g_handler() { static LogHandler h; return h; }
std::atomic<detail::FrameSource>       g_frame_source{nullptr};
std::atomic<detail::ToolsLogForwarder> g_tools_forwarder{nullptr};

const char* level_tag(LogLevel level) noexcept {
    switch (level) {
        case LogLevel::debug: return "debug";
        case LogLevel::info:  return "info";
        case LogLevel::warn:  return "warn";
        case LogLevel::error: return "error";
    }
    return "info";
}

}  // namespace

namespace detail {

void set_log_frame_source(FrameSource fn) noexcept {
    g_frame_source.store(fn, std::memory_order_relaxed);
}

void set_tools_log_forwarder(ToolsLogForwarder fn) noexcept {
    g_tools_forwarder.store(fn, std::memory_order_relaxed);
}

}  // namespace detail

void forward_to_console(const LogRecord& record) {
    // stderr line (greppable, matches the historical trace format), plus
    // the Windows debugger console so a GUI-subsystem app still shows logs.
    std::string line = record.msg.empty() ? std::string{}
                                          : std::string(record.msg);
#ifndef NDEBUG
    std::fprintf(stderr, "[%s] %.*s\n", level_tag(record.level),
                 static_cast<int>(record.msg.size()), record.msg.data());
    std::fflush(stderr);
#else
    // Release: only warnings/errors reach the console by default.
    if (record.level == LogLevel::warn || record.level == LogLevel::error) {
        std::fprintf(stderr, "[%s] %.*s\n", level_tag(record.level),
                     static_cast<int>(record.msg.size()), record.msg.data());
        std::fflush(stderr);
    }
#endif
#if defined(_WIN32)
    if (IsDebuggerPresent()) {
        line.insert(0, "[affineui] ");
        line.push_back('\n');
        OutputDebugStringA(line.c_str());
    }
#else
    (void)line;
#endif
}

void forward_to_affinetools(const LogRecord& record) {
    if (auto fwd = g_tools_forwarder.load(std::memory_order_relaxed)) {
        fwd(record);
    }
}

void set_log_handler(LogHandler handler) {
    const std::lock_guard<std::mutex> lock(handler_mutex());
    g_handler() = std::move(handler);
}

void log(LogLevel level, std::string_view msg) {
    LogRecord record;
    record.level = level;
    record.msg = msg;
    if (auto src = g_frame_source.load(std::memory_order_relaxed)) {
        const auto [frame, t_ms] = src();
        record.frame = frame;
        record.t_ms = t_ms;
    }

    // The active handler (default = console + affinetools).
    LogHandler handler;
    {
        const std::lock_guard<std::mutex> lock(handler_mutex());
        handler = g_handler();
    }
    if (handler) {
        handler(record);
    } else {
        forward_to_console(record);
        forward_to_affinetools(record);
    }

    // Embedder's raw-pointer sink (InitDesc::log) still fires, if set.
    const auto legacy = detail::legacy_log_sink();
    if (legacy.fn) {
        std::string owned(msg);
        legacy.fn(level, owned.c_str(), legacy.user);
    }
}

}  // namespace affineui
