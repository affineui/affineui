#pragma once

// AffineUI diagnostic logging.
//
// AffineUI's own warnings, errors, and parse diagnostics flow through one
// funnel. By default they go to BOTH the platform debug console (stderr,
// plus OutputDebugString on Windows) AND — when an affinetools viewer is
// attached — the devtools log panel, where each line is stamped with the
// frame it arrived on. A host can replace the handler entirely to route
// diagnostics wherever it likes.
//
// This is for AFFINEUI's diagnostics. Application logging stays the app's
// own concern; an app that wants its lines in the panel too can call
// affineui::log() explicitly.
//
// (The older InitDesc::log / LogFn raw-pointer sink still works for
// embedders — it is invoked in addition to the handler below.)

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

#include "affineui/embed.h"  // LogLevel

namespace affineui {

/// One diagnostic line. `frame` is the presented-frame index current when
/// the line was emitted (0 before the first frame), and `t_ms` is the time
/// since app start — so the devtools panel can align a log line with the
/// performance graph. `msg` is owned for the duration of the handler call
/// only; copy it to retain.
struct LogRecord {
    LogLevel         level{LogLevel::info};
    std::string_view msg;
    std::uint64_t    frame{0};
    double           t_ms{0.0};
};

using LogHandler = std::function<void(const LogRecord&)>;

/// Install a diagnostics handler. Passing an empty handler restores the
/// default (console + attached affinetools). The handler is called on the
/// thread that emitted the log — usually the app/UI thread.
///
/// A custom handler fully OWNS what happens to each line: it may write to
/// its own logging system, the console, both, or neither. If it wants the
/// affinetools panel to keep receiving lines, it calls
/// `forward_to_affinetools(record)` itself (most custom handlers do). The
/// two default sinks are exposed as primitives so a handler can reuse them.
void set_log_handler(LogHandler handler);

/// Default-handler primitives, callable from a custom handler:
///   • forward_to_console  — stderr (+ OutputDebugString on Windows).
///   • forward_to_affinetools — the attached devtools log ring (no-op when
///     nothing is attached / the perf subsystem is compiled out).
void forward_to_console(const LogRecord& record);
void forward_to_affinetools(const LogRecord& record);

/// Emit a diagnostic line through the active handler. Used by AffineUI
/// internally; apps may call it to surface their own lines in the same
/// stream (and the devtools panel).
void log(LogLevel level, std::string_view msg);

/// Convenience overloads.
inline void log_info(std::string_view msg)  { log(LogLevel::info, msg); }
inline void log_warn(std::string_view msg)  { log(LogLevel::warn, msg); }
inline void log_error(std::string_view msg) { log(LogLevel::error, msg); }

}  // namespace affineui
