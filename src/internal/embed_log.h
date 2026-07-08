#pragma once

// Tiny internal log sink. Embedders install a LogFn via InitDesc; until then
// messages go to stderr in debug builds and are dropped otherwise. Used in
// place of bare fprintf(stderr, ...) so a host can capture our diagnostics.

#include "affineui/embed.h"

namespace affineui::detail {

void set_log_sink(LogFn fn, void* user) noexcept;
void log_msg(LogLevel level, const char* msg) noexcept;

// The embedder's raw-pointer sink, exposed for the log facility (log.cpp)
// so it keeps firing alongside the std::function handler.
LogFn legacy_log_fn() noexcept;
void* legacy_log_user() noexcept;

}  // namespace affineui::detail
