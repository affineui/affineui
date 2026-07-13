#pragma once

// Tiny internal log sink. Embedders install a LogFn via InitDesc; until then
// messages go to stderr in debug builds and are dropped otherwise. Used in
// place of bare fprintf(stderr, ...) so a host can capture our diagnostics.

#include "affineui/embed.h"

namespace affineui::detail {

std::uint64_t set_log_sink(LogFn fn, void* user) noexcept;
void clear_log_sink(std::uint64_t registration) noexcept;
void log_msg(LogLevel level, const char* msg) noexcept;

// The embedder's sink, snapshotted as one pair so concurrent replacement can
// never combine one registration's function with another's user pointer.
struct LegacyLogSink {
    LogFn fn{nullptr};
    void* user{nullptr};
};
LegacyLogSink legacy_log_sink() noexcept;

}  // namespace affineui::detail
