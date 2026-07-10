// affinetools dom/css/resource read domain — the app-thread half of the
// devtools request path (docs/AFFINETOOLS_DESIGN.md §2.1/§3.4, schemas in
// docs/AFFINETOOLS_PROTOCOL.md).
//
// tools_pump(Document&) drains the typed commands the server thread
// queued (src/core/tools/tools_commands.h), services them against the
// live document, and hands complete response documents back for the
// server thread to frame and send. Runs once per frame on the app
// thread — including idle-short-circuited frames, so a quiet target
// still answers.
//
// Contract (DESIGN §2.4): every read is read-only-never-relayout. Node
// geometry comes from the last-laid-out Block bounds; nothing here
// mutates the DOM, forces a restyle, or triggers the hidden relayout
// find_element_rect is documented to perform.

#include "affineui/document.h"
#include "affineui/tools.h"

#if AFFINEUI_TOOLS

#include "core/tools/tools_commands.h"
#include "renderer/dom/document_impl.h"

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

namespace affineui {

#if !defined(AFFINEUI_STUB_BUILD)

namespace {

using detail::DocumentImpl;
using detail::ToolsCommand;

constexpr std::size_t kMaxChildren = 512;          // per dom.children reply
constexpr std::size_t kMaxTextPreview = 160;       // text-node preview bytes
constexpr std::size_t kMaxHtmlBytes = 512u * 1024; // dom.html budget
constexpr std::size_t kMaxSheetBytes = 768u * 1024;

std::string error_response(long long id, int code, const char* message) {
    char buf[160];
    std::snprintf(buf, sizeof(buf),
                  "{\"id\":%lld,\"error\":{\"code\":%d,\"message\":\"%s\"}}",
                  id, code, message);
    return buf;
}

std::string result_response(long long id, std::string_view result_json) {
    std::string out;
    out.reserve(result_json.size() + 32);
    char head[32];
    std::snprintf(head, sizeof(head), "{\"id\":%lld,\"result\":", id);
    out += head;
    out += result_json;
    out += '}';
    return out;
}

/// Weak handle for an arbitrary DOM node (the by-id variant is
/// Document::weak_handle_for_id; same slot-reuse discipline). Linear in
/// the slot count — acceptable for on-demand tool reads, which create
/// slots only as the user browses.
DomHandle handle_for_node(DocumentImpl& impl, lxb_dom_node_t* node) {
    DomHandle out{};
    if (node == nullptr) return out;
    out.document_id = impl.document_id;
    for (std::size_t i = 0; i < impl.dom_weak_slots.size(); ++i) {
        auto& slot = impl.dom_weak_slots[i];
        if (slot.node == node) {
            out.node_slot = static_cast<std::uint32_t>(i + 1);
            out.generation = slot.generation;
            return out;
        }
    }
    std::size_t slot_index = impl.dom_weak_slots.size();
    for (std::size_t i = 0; i < impl.dom_weak_slots.size(); ++i) {
        if (impl.dom_weak_slots[i].node == nullptr) {
            slot_index = i;
            break;
        }
    }
    if (slot_index == impl.dom_weak_slots.size()) {
        impl.dom_weak_slots.push_back({});
    }
    auto& slot = impl.dom_weak_slots[slot_index];
    slot.node = node;
    if (slot.generation == 0) slot.generation = 1;
    out.node_slot = static_cast<std::uint32_t>(slot_index + 1);
    out.generation = slot.generation;
    return out;
}

/// Resolve a wire nid back to a node; nullptr when stale (the
/// versioned-slot check — same rules as Document::weak_handle_valid).
lxb_dom_node_t* resolve_handle(DocumentImpl& impl, DomHandle h) {
    if (h.document_id != impl.document_id || h.node_slot == 0) return nullptr;
    const std::size_t index = static_cast<std::size_t>(h.node_slot - 1);
    if (index >= impl.dom_weak_slots.size()) return nullptr;
    const auto& slot = impl.dom_weak_slots[index];
    if (slot.generation != h.generation) return nullptr;
    return slot.node;
}

void append_nid(std::string& out, const DomHandle& h) {
    char buf[48];
    std::snprintf(buf, sizeof(buf), "[%u,%u,%u]", h.document_id, h.node_slot,
                  h.generation);
    out += buf;
}

void append_rect(std::string& out, const Rect& r) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "[%d,%d,%d,%d]", r.x, r.y, r.w, r.h);
    out += buf;
}

/// A child worth showing in the Elements tree: element nodes, plus text
/// nodes that aren't pure collapsible whitespace.
bool is_tree_child(lxb_dom_node_t* node) {
    if (node->type == LXB_DOM_NODE_TYPE_ELEMENT) return true;
    if (node->type == LXB_DOM_NODE_TYPE_TEXT) {
        return !detail::node_is_collapsible_whitespace(node);
    }
    return false;
}

std::size_t count_tree_children(lxb_dom_node_t* node) {
    std::size_t n = 0;
    for (lxb_dom_node_t* c = lxb_dom_node_first_child(node); c != nullptr;
         c = lxb_dom_node_next(c)) {
        if (is_tree_child(c)) ++n;
    }
    return n;
}

/// Last-laid-out border-box (visual space: scroll + transform applied)
/// for an element; zeroed when the element has no laid-out block
/// (display:none subtree member, <head> content, …).
Rect element_rect(const DocumentImpl& impl, lxb_dom_element_t* elem) {
    const int idx = detail::block_index_for_exact_element(impl, elem);
    if (idx < 0) return Rect{};
    return detail::block_border_visual_rect(impl, idx);
}

void truncate_utf8(std::string& s, std::size_t max_bytes) {
    if (s.size() <= max_bytes) return;
    std::size_t cut = max_bytes;
    // Don't split a UTF-8 sequence.
    while (cut > 0 && (static_cast<unsigned char>(s[cut]) & 0xC0u) == 0x80u) {
        --cut;
    }
    s.resize(cut);
}

/// One tree node object: {"nid":[…],"tag":"div","children":N,
/// "rect":[…],"attrs":[["class","x"],…]} — text nodes use tag "#text"
/// and carry "text" instead of attrs/rect.
void append_node_json(std::string& out, DocumentImpl& impl,
                      lxb_dom_node_t* node) {
    out += "{\"nid\":";
    append_nid(out, handle_for_node(impl, node));
    if (node->type == LXB_DOM_NODE_TYPE_TEXT) {
        std::string text = detail::node_text(node);
        truncate_utf8(text, kMaxTextPreview);
        out += ",\"tag\":\"#text\",\"children\":0,\"text\":\"";
        detail::tools_json_escape(out, text);
        out += "\"}";
        return;
    }
    auto* elem = lxb_dom_interface_element(node);
    out += ",\"tag\":\"";
    detail::tools_json_escape(out, detail::tag_view(elem));
    char buf[32];
    std::snprintf(buf, sizeof(buf), "\",\"children\":%zu,\"rect\":",
                  count_tree_children(node));
    out += buf;
    append_rect(out, element_rect(impl, elem));
    out += ",\"attrs\":[";
    bool first = true;
    for (const auto& [name, value] : detail::element_attrs(elem)) {
        if (!first) out += ',';
        first = false;
        out += "[\"";
        detail::tools_json_escape(out, name);
        out += "\",\"";
        detail::tools_json_escape(out, value);
        out += "\"]";
    }
    out += "]}";
}

std::string serve_dom_document(DocumentImpl& impl, long long id) {
    lxb_dom_node_t* root = detail::document_dom_root(impl);
    if (root == nullptr) return error_response(id, -32011, "no document");
    std::string result = "{\"root\":";
    append_node_json(result, impl, root);
    result += '}';
    return result_response(id, result);
}

std::string serve_dom_children(DocumentImpl& impl, long long id,
                               DomHandle nid) {
    lxb_dom_node_t* node = resolve_handle(impl, nid);
    if (node == nullptr) return error_response(id, -32010, "stale node");
    std::string result = "{\"nodes\":[";
    std::size_t n = 0;
    bool truncated = false;
    for (lxb_dom_node_t* c = lxb_dom_node_first_child(node); c != nullptr;
         c = lxb_dom_node_next(c)) {
        if (!is_tree_child(c)) continue;
        if (n == kMaxChildren) {
            truncated = true;
            break;
        }
        if (n > 0) result += ',';
        append_node_json(result, impl, c);
        ++n;
    }
    result += truncated ? "],\"truncated\":true}" : "]}";
    return result_response(id, result);
}

std::string serve_dom_html(DocumentImpl& impl, long long id, DomHandle nid) {
    lxb_dom_node_t* node =
        nid ? resolve_handle(impl, nid) : detail::document_dom_root(impl);
    if (node == nullptr) {
        return error_response(id, nid ? -32010 : -32011,
                              nid ? "stale node" : "no document");
    }
    std::string html;
    auto append = [](const lxb_char_t* data, size_t len,
                     void* ctx) -> lxb_status_t {
        auto* out = static_cast<std::string*>(ctx);
        if (out->size() < kMaxHtmlBytes) {
            out->append(reinterpret_cast<const char*>(data), len);
        }
        return LXB_STATUS_OK;
    };
    (void)lxb_html_serialize_tree_cb(node, append, &html);
    bool truncated = false;
    if (html.size() > kMaxHtmlBytes) {
        truncate_utf8(html, kMaxHtmlBytes);
        truncated = true;
    }
    std::string result;
    result.reserve(html.size() + 48);
    result += "{\"html\":\"";
    detail::tools_json_escape(result, html);
    result += truncated ? "\",\"truncated\":true}" : "\"}";
    return result_response(id, result);
}

std::string serve_css_box_model(DocumentImpl& impl, long long id,
                                DomHandle nid) {
    lxb_dom_node_t* node = resolve_handle(impl, nid);
    if (node == nullptr || node->type != LXB_DOM_NODE_TYPE_ELEMENT) {
        return error_response(id, -32010, "stale node");
    }
    auto* elem = lxb_dom_interface_element(node);
    const int idx = detail::block_index_for_exact_element(impl, elem);
    if (idx < 0) {
        // No laid-out block (display:none subtree, <head> content, …).
        return result_response(id, "{\"laid_out\":false}");
    }
    const Block& block = impl.blocks[static_cast<std::size_t>(idx)];
    const detail::ComputedStyle& cs = impl.style_store.computed(block.id);
    std::string result;
    result.reserve(256);
    result += "{\"laid_out\":true,\"rect\":";
    append_rect(result, block.bounds);
    result += ",\"visual\":";
    append_rect(result, detail::block_border_visual_rect(impl, idx));
    char buf[192];
    std::snprintf(buf, sizeof(buf),
                  ",\"margin\":[%d,%d,%d,%d],\"border\":[%d,%d,%d,%d],"
                  "\"padding\":[%d,%d,%d,%d],\"scroll_y\":%d,"
                  "\"content_h\":%d}",
                  cs.margin_top, cs.margin_right, cs.margin_bottom,
                  cs.margin_left, cs.used_border_top(), cs.used_border_right(),
                  cs.used_border_bottom(), cs.used_border_left(),
                  cs.padding_top, cs.padding_right, cs.padding_bottom,
                  cs.padding_left, block.scroll_y, block.content_h);
    result += buf;
    return result_response(id, result);
}

/// One enumerable stylesheet source. The list mirrors cascade order:
/// author <style>/<link> sheets in document order, then the user
/// stylesheet (framework bundle / app theme) last.
struct SheetSource {
    std::string origin;  // "style" | "link" | "user"
    std::string label;   // href, or a positional label
    std::string text;    // resolved text ("" until fetched for links)
    bool        text_known{true};
};

std::vector<SheetSource> enumerate_sheets(DocumentImpl& impl) {
    std::vector<SheetSource> out;
    lxb_dom_node_t* root =
        impl.doc ? lxb_dom_interface_node(impl.doc) : nullptr;
    if (root != nullptr) {
        int style_n = 0;
        auto visit = [&](lxb_dom_element_t* elem) {
            const std::string_view tag = detail::tag_view(elem);
            if (tag == "style") {
                SheetSource s;
                s.origin = "style";
                s.label = "<style> #" + std::to_string(++style_n);
                s.text = detail::node_text(
                    lxb_dom_interface_node(elem),
                    detail::ComputedStyle::WhiteSpace::Pre);
                out.push_back(std::move(s));
            } else if (tag == "link") {
                const std::string rel =
                    detail::attr_string(elem, "rel");
                if (rel != "stylesheet") return;
                SheetSource s;
                s.origin = "link";
                s.label = detail::attr_string(elem, "href");
                s.text_known = false;  // resolved on demand, one at a time
                out.push_back(std::move(s));
            }
        };
        detail::walk_dom_elements(root, visit);
    }
    if (!impl.user_stylesheet.empty()) {
        SheetSource s;
        s.origin = "user";
        s.label = impl.user_stylesheet_base_url.empty()
                      ? std::string{"user stylesheet"}
                      : impl.user_stylesheet_base_url;
        s.text = impl.user_stylesheet;
        out.push_back(std::move(s));
    }
    return out;
}

std::string serve_resource_stylesheets(DocumentImpl& impl, long long id) {
    const std::vector<SheetSource> sheets = enumerate_sheets(impl);
    std::string result = "{\"sheets\":[";
    for (std::size_t i = 0; i < sheets.size(); ++i) {
        if (i > 0) result += ',';
        char buf[48];
        std::snprintf(buf, sizeof(buf), "{\"index\":%zu,\"origin\":\"", i);
        result += buf;
        result += sheets[i].origin;  // fixed vocabulary — no escaping needed
        result += "\",\"label\":\"";
        detail::tools_json_escape(result, sheets[i].label);
        // bytes: -1 = unknown until fetched (unresolved <link>).
        std::snprintf(buf, sizeof(buf), "\",\"bytes\":%lld}",
                      sheets[i].text_known
                          ? static_cast<long long>(sheets[i].text.size())
                          : -1ll);
        result += buf;
    }
    result += "]}";
    return result_response(id, result);
}

std::string serve_resource_stylesheet_text(DocumentImpl& impl, long long id,
                                           int index) {
    std::vector<SheetSource> sheets = enumerate_sheets(impl);
    if (index < 0 || static_cast<std::size_t>(index) >= sheets.size()) {
        return error_response(id, -32012, "no such stylesheet");
    }
    SheetSource& sheet = sheets[static_cast<std::size_t>(index)];
    if (!sheet.text_known) {
        // Resolve just the requested <link> sheet. resource_loader is an
        // app-thread callback (same contract as the renderer's own asset
        // loading), so it runs here in the pump — one sheet, on demand,
        // never the whole list. Loaders are expected to be local-disk
        // fast; a network-backed loader would need an async resource
        // domain (tracked with the R3 registries), not a thread hop that
        // the callback's contract doesn't allow.
        sheet.text = impl.resource_loader && !sheet.label.empty()
                         ? impl.resource_loader(sheet.label)
                         : std::string{};
        sheet.text_known = true;
    }
    bool truncated = false;
    if (sheet.text.size() > kMaxSheetBytes) {
        truncate_utf8(sheet.text, kMaxSheetBytes);
        truncated = true;
    }
    std::string result;
    result.reserve(sheet.text.size() + 64);
    char head[32];
    std::snprintf(head, sizeof(head), "{\"index\":%d,\"text\":\"", index);
    result += head;
    detail::tools_json_escape(result, sheet.text);
    result += truncated ? "\",\"truncated\":true}" : "\"}";
    return result_response(id, result);
}

std::string service_command(DocumentImpl& impl, const ToolsCommand& cmd) {
    switch (cmd.kind) {
        case ToolsCommand::Kind::DomDocument:
            return serve_dom_document(impl, cmd.id);
        case ToolsCommand::Kind::DomChildren:
            return serve_dom_children(impl, cmd.id, cmd.nid);
        case ToolsCommand::Kind::DomHtml:
            return serve_dom_html(impl, cmd.id, cmd.nid);
        case ToolsCommand::Kind::CssBoxModel:
            return serve_css_box_model(impl, cmd.id, cmd.nid);
        case ToolsCommand::Kind::ResourceStylesheets:
            return serve_resource_stylesheets(impl, cmd.id);
        case ToolsCommand::Kind::ResourceStylesheetText:
            return serve_resource_stylesheet_text(impl, cmd.id, cmd.index);
    }
    return error_response(cmd.id, -32601, "unknown method");
}

}  // namespace

void tools_pump(Document& doc) {
    if (!detail::tools_has_pending_commands()) return;
    std::vector<detail::ToolsCommand> cmds;
    if (!detail::tools_take_commands(cmds)) return;
    detail::DocumentImpl& impl = *doc.impl_;
    for (const detail::ToolsCommand& cmd : cmds) {
        detail::tools_push_response(cmd.epoch, service_command(impl, cmd));
    }
}

#else  // AFFINEUI_STUB_BUILD — no lexbor: answer every read with an error.

void tools_pump(Document&) {
    if (!detail::tools_has_pending_commands()) return;
    std::vector<detail::ToolsCommand> cmds;
    if (!detail::tools_take_commands(cmds)) return;
    for (const detail::ToolsCommand& cmd : cmds) {
        char buf[128];
        std::snprintf(buf, sizeof(buf),
                      "{\"id\":%lld,\"error\":{\"code\":-32011,"
                      "\"message\":\"no dom in this build\"}}",
                      cmd.id);
        detail::tools_push_response(cmd.epoch, buf);
    }
}

#endif  // AFFINEUI_STUB_BUILD

}  // namespace affineui

#endif  // AFFINEUI_TOOLS
