#pragma once

// Internal diagnostics substrate (see docs/TRACING_AND_PERFORMANCE_LOGGING.md).

namespace affineui::detail {

/// AFFINEUI_SAMPLER=1: start the in-process sampling profiler against the
/// CALLING thread (App::run invokes this on the UI thread). A watchdog
/// thread samples at ~1 kHz via the OS unwinder and appends aggregated
/// top stacks (exe-relative RVAs) to affineui_profile.txt in the working
/// directory every 5 s. Symbolize offline with
///   llvm-symbolizer --obj=<exe> <0x140000000 + RVA>
/// No-op when the env var is unset or on non-win32 platforms. Idempotent.
void maybe_start_stack_sampler();

/// AFFINEUI_TRACE_JSON=1: emit Chrome-trace-format spans (view in
/// chrome://tracing or Perfetto) to affineui_trace.json in the working
/// directory. Complete "X" events are written when a span closes; the
/// file is valid the moment the process dies (Chrome accepts an
/// unterminated event array).
bool trace_json_enabled();
void trace_json_complete(const char* name, long long ts_us,
                         long long dur_us);

/// RAII span for trace_json: measures construction→destruction and emits
/// one complete event. Near-zero cost when tracing is off.
class TraceSpan {
public:
    explicit TraceSpan(const char* name);
    ~TraceSpan();
    TraceSpan(const TraceSpan&)            = delete;
    TraceSpan& operator=(const TraceSpan&) = delete;

private:
    const char* name_;
    long long   t0_us_{0};
    bool        on_{false};
};

}  // namespace affineui::detail
