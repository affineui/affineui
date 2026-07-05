#pragma once

// Per-frame telemetry (data plane R0/R1 — docs/TRACING_AND_PERFORMANCE_LOGGING.md,
// schema in docs/AFFINETOOLS_PROTOCOL.md).
//
// One FrameTelemetry record is assembled per *presented* frame in the App
// frame callback: wall-clock frame gap (the truth metric — a stalled
// pipeline shows in gaps even when per-frame durations look healthy),
// stage timings and op/dirty counters from RenderStats, and allocator
// deltas from affineui::mem. The same record is readable in-process
// (App::frame_telemetry()), streamed to a JSONL file
// (AFFINEUI_TELEMETRY=<path>, sampled by AFFINEUI_TELEMETRY_EVERY=N), and
// will ride the affinetools wire protocol as telemetry.frame.
//
// Gating (TRACING §1.6/§1.7): AFFINEUI_PERF is the master compile switch
// for the perf/monitoring subsystem. Define AFFINEUI_PERF=0 before
// compiling the library to strip the implementation — the struct and the
// accessors below remain declared (host code compiles unchanged) but
// become zero-cost inline stubs, and no formatting/sink code exists in
// the build. Compiled in, the sink gates on one relaxed atomic bool;
// call sites must branch on telemetry::sink_active() so disabled paths
// never compute arguments.

#include <cstddef>
#include <cstdint>

#include "affineui/memory.h"  // mem::Stats (cheap, always-compiled counters)

#ifndef AFFINEUI_PERF
#define AFFINEUI_PERF 1
#endif

namespace affineui {

struct RenderStats;  // renderer.h

/// Telemetry record schema version — bumped on breaking field changes
/// (see docs/AFFINETOOLS_PROTOCOL.md for the versioning contract).
inline constexpr std::uint32_t kTelemetrySchemaVersion = 1;

struct FrameTelemetry {
    std::uint32_t v{kTelemetrySchemaVersion};

    // ── identity / wall clock ────────────────────────────────────────────
    std::uint64_t frame{0};    ///< presented-frame index (monotonic, 1-based)
    double        t_ms{0.0};   ///< since session t0, at callback entry
    double        gap_ms{0.0}; ///< wall-clock gap since previous callback entry
    double        cb_ms{0.0};  ///< this callback's duration
    std::uint64_t skipped{0};  ///< idle short-circuits since last presented frame

    // ── viewport ─────────────────────────────────────────────────────────
    int   fb_w{0};
    int   fb_h{0};
    float dpi{0.0f};

    // ── stage timings, µs (RenderStats) ──────────────────────────────────
    std::uint32_t prep_us{0};
    std::uint32_t layout_us{0};
    std::uint32_t record_us{0};
    std::uint32_t raster_us{0};
    std::uint32_t composite_us{0};

    // ── display-list / dirty ops (RenderStats) ───────────────────────────
    std::uint32_t cached_ops{0};
    std::uint32_t culled_ops{0};
    std::uint32_t changed_ops{0};
    std::uint32_t dirty_rects{0};
    std::uint32_t dirty_pct_x100{0};  ///< dirty viewport area, percent × 100

    // ── state flags (RenderStats) ────────────────────────────────────────
    bool recorded{false};
    bool dl_changed{false};
    bool rasterized{false};
    bool partial{false};
    bool direct{false};
    bool layer_reused{false};
    bool animations{false};

    // ── allocator (affineui::mem seam; global new/delete lands with R2) ──
    std::uint64_t mem_live_bytes{0};
    std::uint64_t mem_live_blocks{0};
    std::uint32_t allocs{0};  ///< delta since previous frame record
    std::uint32_t frees{0};   ///< delta since previous frame record
};

#if AFFINEUI_PERF

/// Copy the per-frame RenderStats fields into `t`. Pure field mapping —
/// unit-testable without a renderer.
void fill_from_render_stats(FrameTelemetry& t, const RenderStats& rs) noexcept;

/// Fill live sizes from `cur` and alloc/free deltas from `cur - prev`.
void fill_mem_delta(FrameTelemetry& t,
                    const mem::Stats& prev,
                    const mem::Stats& cur) noexcept;

// ── JSONL formatting (allocation-free; docs/AFFINETOOLS_PROTOCOL.md) ─────
// Each writes one record WITHOUT a trailing newline and returns its length,
// or 0 when `cap` is too small. 512 bytes is always sufficient.

std::size_t format_session_jsonl(char* buf, std::size_t cap);
std::size_t format_frame_jsonl(const FrameTelemetry& t,
                               char* buf, std::size_t cap);
std::size_t format_idle_jsonl(double t_ms, std::uint64_t skipped,
                              char* buf, std::size_t cap);

namespace telemetry {

/// Open the JSONL sink from AFFINEUI_TELEMETRY / AFFINEUI_TELEMETRY_EVERY.
/// Idempotent; returns whether a sink is active. Called by App startup;
/// embedded hosts may call it directly.
bool sink_open_from_env();

/// Fast gate — one relaxed atomic load. Branch on this at call sites so
/// disabled paths never compute record contents (TRACING §1.7).
[[nodiscard]] bool sink_active() noexcept;

/// Write one frame record (applies AFFINEUI_TELEMETRY_EVERY sampling).
void sink_write_frame(const FrameTelemetry& t);

/// Write one idle heartbeat (never sampled out).
void sink_write_idle(double t_ms, std::uint64_t skipped);

/// Flush and close (also registered atexit).
void sink_close();

}  // namespace telemetry

#else  // AFFINEUI_PERF == 0 — declared-but-stubbed (TRACING §1.6)

inline void fill_from_render_stats(FrameTelemetry&, const RenderStats&) noexcept {}
inline void fill_mem_delta(FrameTelemetry&, const mem::Stats&,
                           const mem::Stats&) noexcept {}
inline std::size_t format_session_jsonl(char*, std::size_t) { return 0; }
inline std::size_t format_frame_jsonl(const FrameTelemetry&, char*,
                                      std::size_t) { return 0; }
inline std::size_t format_idle_jsonl(double, std::uint64_t, char*,
                                     std::size_t) { return 0; }

namespace telemetry {
inline bool sink_open_from_env() { return false; }
[[nodiscard]] inline bool sink_active() noexcept { return false; }
inline void sink_write_frame(const FrameTelemetry&) {}
inline void sink_write_idle(double, std::uint64_t) {}
inline void sink_close() {}
}  // namespace telemetry

#endif  // AFFINEUI_PERF

}  // namespace affineui
