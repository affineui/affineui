#pragma once

// affineui_tools — the affinetools sidecar library (out-of-process
// devtools; docs/AFFINETOOLS_DESIGN.md §4). This is the CLIENT half:
// target discovery, the wire protocol (docs/AFFINETOOLS_PROTOCOL.md),
// a live telemetry model, and dump-file summaries. The affinetools
// viewer executable (tools/affinetools) is a thin shell over this; the
// panel components accrete here as S2+ land.
//
// Deliberately NOT part of the core library: the probed target needs
// only core (server side); this library is for tools/viewer processes.
// Single-file oriented — it must stay amalgamatable to one header/cpp
// pair beside affineui.h.

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "affineui/telemetry.h"

namespace affineui::tools {

/// One attachable target, read from <tempdir>/affineui-tools/<pid>.json.
struct TargetInfo {
    int         pid{0};
    int         port{0};
    std::string token;
    std::string exe;
    std::string affineui_version;
};

/// All advertised targets, newest discovery file first. Stale files from
/// crashed targets are included — they simply fail to connect.
[[nodiscard]] std::vector<TargetInfo> discover_targets();

/// One diagnostic line from the target's log stream. `frame` is the
/// presented-frame index the line was emitted on (0 if pre-first-frame),
/// so the log panel can jump the performance graph to it on click.
struct LogEntry {
    int           level{1};   // 0 debug, 1 info, 2 warn, 3 error
    std::uint64_t frame{0};
    double        t_ms{0.0};
    std::string   text;
};

/// Thread-safe snapshot of everything the client knows about its target.
struct ClientStatus {
    bool        connected{false};   ///< socket up + hello accepted
    int         pid{0};
    std::string exe;
    std::string affineui_version;
    std::string session_id;

    bool           have_frame{false};
    FrameTelemetry last_frame{};    ///< latest telemetry.frame params
    bool           last_was_idle{false};
    double         idle_t_ms{0.0};
    std::uint64_t  idle_skipped{0};

    std::uint64_t events{0};        ///< total events received
    std::uint64_t dropped{0};       ///< sum of telemetry.dropped counts
};

/// Protocol client. attach() performs the hello handshake + telemetry
/// subscribe synchronously, then a reader thread keeps the model fresh.
/// Poll revision() from the UI loop: it bumps on every model change, so
/// "rebuild the view when it moved" is one integer compare.
class Client {
public:
    Client();
    ~Client();
    Client(const Client&)            = delete;
    Client& operator=(const Client&) = delete;

    bool attach(const TargetInfo& target);
    bool attach_pid(int pid);
    /// Newest advertised target that accepts a connection.
    bool attach_newest();
    void detach();

    [[nodiscard]] bool          attached() const noexcept;
    [[nodiscard]] std::uint64_t revision() const noexcept;
    [[nodiscard]] ClientStatus  status() const;

    /// Copy the most recent frame records (oldest first, up to `max`) into
    /// `out` — the scrolling frametime graph's data source. Thread-safe.
    void frame_history(std::vector<FrameTelemetry>& out,
                       std::size_t max = 240) const;

    /// Copy the most recent log lines (oldest first, up to `max`) into
    /// `out` — the log panel's data source. Thread-safe.
    void log_history(std::vector<LogEntry>& out, std::size_t max = 4096) const;

    /// Total log lines received this session (for the panel's scroll math
    /// and the "N lines" readout), and lines dropped to the budget.
    [[nodiscard]] std::size_t   log_count() const noexcept;
    [[nodiscard]] std::uint64_t log_dropped() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

/// Summary statistics over a span of frame records — the frametime
/// graph's point/range selection readout. `over_budget` counts frames
/// whose wall-clock gap exceeded `budget_ms` (default 6.94 ms = 144 Hz).
/// Per-component averages are the stage timings; `other` is the
/// wall-clock gap minus the summed stages (idle wait + untracked cost).
struct RangeStats {
    std::size_t frames{0};
    double      avg_fps{0.0};
    double      avg_gap_ms{0.0};
    double      min_gap_ms{0.0};
    double      max_gap_ms{0.0};
    double      p95_gap_ms{0.0};
    std::size_t over_budget{0};
    double      budget_ms{6.94};
    // Average per-component contribution across the span (ms).
    double      avg_layout_ms{0.0};
    double      avg_raster_ms{0.0};
    double      avg_composite_ms{0.0};
    double      avg_other_ms{0.0};
    double      avg_cb_ms{0.0};
};

/// Compute RangeStats over frames[begin, end) (end clamped to size).
/// begin==end (or an empty span) yields a zeroed result with frames==0.
[[nodiscard]] RangeStats compute_range_stats(
    const std::vector<FrameTelemetry>& frames,
    std::size_t begin, std::size_t end, double budget_ms = 6.94);

/// Post-mortem: summarize a telemetry JSONL dump (a recorded session —
/// AFFINEUI_TELEMETRY output or `affinetools_cli.py dump`).
struct DumpSummary {
    bool        ok{false};
    std::string error;
    std::string affineui_version;
    std::size_t frames{0};
    std::size_t idles{0};
    double      avg_gap_ms{0.0};
    double      p95_gap_ms{0.0};
    double      max_gap_ms{0.0};
    double      avg_cb_ms{0.0};
};

[[nodiscard]] DumpSummary summarize_dump(const std::string& path);

}  // namespace affineui::tools
