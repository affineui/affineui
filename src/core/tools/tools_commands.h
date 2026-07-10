#pragma once

// affinetools command bridge — the typed hand-off between the protocol
// server thread and the app thread (AFFINETOOLS_DESIGN.md §2.1).
//
// The server thread parses inbound JSON into ToolsCommand values and
// enqueues them (bounded; overflow answers an immediate "busy" error on
// the server thread). The app thread drains the queue once per frame in
// affineui::tools_pump(Document&) — the only place DOM state is read —
// and pushes complete, pre-formatted response documents back for the
// server thread to frame and send. The app thread never touches the
// socket; the server thread never touches the DOM.
//
// Everything here is inside the AFFINEUI_TOOLS master switch; the header
// is only included by gated TUs (tools_server.cpp, document_tools.cpp).

#include <cstdint>
#include <string>
#include <vector>

#include "affineui/types.h"

namespace affineui::detail {

struct ToolsCommand {
    enum class Kind : std::uint8_t {
        DomDocument,             // → {root: Node}
        DomChildren,             // nid → {nodes:[Node…]}
        DomHtml,                 // nid (or invalid = whole doc) → {html}
        CssBoxModel,             // nid → {rect, visual, margin/border/padding}
        ResourceStylesheets,     // → {sheets:[{index,origin,label,bytes}…]}
        ResourceStylesheetText,  // index → {index,text,truncated}
    };
    long long id{-1};   // wire request id (echoed in the response)
    Kind      kind{Kind::DomDocument};
    DomHandle nid{};    // DomDocument/DomHtml accept an empty handle
    int       index{0}; // ResourceStylesheetText
};

// ── implemented in tools_server.cpp ──────────────────────────────────────

/// App thread: move all queued commands into `out` (appended). Returns
/// true when anything was taken.
bool tools_take_commands(std::vector<ToolsCommand>& out);

/// App thread: queue one complete response document
/// (`{"id":N,"result":…}` or `{"id":N,"error":…}`) for the server thread
/// to frame and send. Dropped if the client is gone.
void tools_push_response(std::string json);

/// Any thread: cheap "are commands waiting" check (one relaxed atomic
/// load) — lets the idle path skip the queue mutex entirely.
[[nodiscard]] bool tools_has_pending_commands() noexcept;

/// JSON string-escape `s` into `out` (shared with the server's own
/// formatting; defined in tools_server.cpp).
void tools_json_escape(std::string& out, std::string_view s);

}  // namespace affineui::detail
