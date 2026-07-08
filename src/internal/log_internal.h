#pragma once

// Internal hooks into the log facility (log.cpp). Not public API.
//   • set_log_frame_source — App installs a callback so every log line is
//     stamped with the current presented-frame index + t_ms.
//   • set_tools_log_forwarder — the affinetools server registers a
//     forwarder so log lines reach the devtools ring, without the log
//     facility depending on the tools code.

#include <cstdint>
#include <utility>

#include "affineui/log.h"

namespace affineui::detail {

using FrameSource = std::pair<std::uint64_t, double> (*)();
void set_log_frame_source(FrameSource fn) noexcept;

using ToolsLogForwarder = void (*)(const LogRecord&);
void set_tools_log_forwarder(ToolsLogForwarder fn) noexcept;

}  // namespace affineui::detail
