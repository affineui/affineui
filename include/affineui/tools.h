#pragma once

// affinetools protocol server — the in-target half of the out-of-process
// devtools (docs/AFFINETOOLS_DESIGN.md §2–3; wire schema in
// docs/AFFINETOOLS_PROTOCOL.md).
//
// The server is process-global and fully decoupled from App lifetime: one
// background thread owns the loopback socket, parses inbound JSON into
// typed commands, and formats/sends outbound events. The app thread only
// ever copies small records into a bounded ring (push_frame / push_idle)
// — it never blocks on the transport and never allocates in proportion to
// client slowness (ring overflow drops oldest and surfaces a dropped
// count on the wire).
//
// Attach model (S1): tools_listen() binds 127.0.0.1 (port 0 = ephemeral)
// and writes a discovery file <temp>/affineui-tools/<pid>.json containing
// {port, token, pid, exe, affineui}. Clients must present the token in
// `hello` before any other method is accepted; a wrong token disconnects.
//
// Gating: entirely inside the AFFINEUI_PERF master switch (TRACING §1.6);
// when compiled out these are inline no-op stubs. Compiled in but not
// listening, the per-frame cost is one relaxed atomic load.

#include <cstdint>

#include "affineui/telemetry.h"

namespace affineui {

#if AFFINEUI_PERF

/// Start the protocol server (idempotent). `port` 0 binds an ephemeral
/// port. Returns false when the socket/bind/thread setup fails — the app
/// keeps running; tools just aren't attachable.
bool tools_listen(int port = 0);

/// Honor AFFINEUI_TOOLS_LISTEN: unset/empty/"0" → no-op (returns false),
/// "1" → tools_listen(0), "<port>" → tools_listen(port). Called by App
/// startup; embedded hosts call it (or tools_listen) themselves.
bool tools_listen_from_env();

/// True while the server is running (listening or serving a client).
[[nodiscard]] bool tools_active() noexcept;

/// The bound port (0 when not listening). For tests and discovery.
[[nodiscard]] int tools_port() noexcept;

/// Stop the server: close sockets, join the thread, remove the discovery
/// file. Safe to call at any time (also registered atexit).
void tools_shutdown();

/// Chrome-DevTools-style "open the tools on this app", zero configuration:
/// ensure the server is listening (starting it if needed), locate the
/// affinetools viewer executable, and launch it attached to this process.
/// Search order: AFFINEUI_TOOLS_EXE (optional override) → beside the
/// current executable → the build tree (tools/affinetools/, walking up
/// from the exe directory) → the system PATH. App binds this to F12 /
/// Ctrl+Shift+I by default (Config::devtools_hotkey). Returns false when
/// no viewer executable could be found or spawned (a message goes to
/// stderr); repeated calls within a debounce window are ignored.
bool tools_open_devtools();

namespace tools {

/// One relaxed atomic load: is an authenticated client subscribed to
/// telemetry? Branch on this before assembling anything for push_frame —
/// disabled paths must not compute arguments (TRACING §1.7).
[[nodiscard]] bool wants_telemetry() noexcept;

/// Queue the presented frame's record for the wire (bounded ring,
/// drop-oldest). Called from the app thread; copies the record.
void push_frame(const FrameTelemetry& t);

/// Queue an idle heartbeat (target.idle).
void push_idle(double t_ms, std::uint64_t skipped);

}  // namespace tools

#else  // AFFINEUI_PERF == 0 — declared-but-stubbed (TRACING §1.6)

inline bool tools_listen(int = 0) { return false; }
inline bool tools_listen_from_env() { return false; }
[[nodiscard]] inline bool tools_active() noexcept { return false; }
[[nodiscard]] inline int tools_port() noexcept { return 0; }
inline void tools_shutdown() {}
inline bool tools_open_devtools() { return false; }

namespace tools {
[[nodiscard]] inline bool wants_telemetry() noexcept { return false; }
inline void push_frame(const FrameTelemetry&) {}
inline void push_idle(double, std::uint64_t) {}
}  // namespace tools

#endif  // AFFINEUI_PERF

}  // namespace affineui
