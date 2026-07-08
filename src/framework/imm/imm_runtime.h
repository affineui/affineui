#pragma once

// ImmRuntime — the per-Document data backing immediate-mode UI.
//
// Holds:
//   • a pointer to the live lxb_html_document_t (we mutate its body
//     directly during view-fn execution; no HTML string round-trip)
//   • the view function the embedder mounted
//   • a dirty flag flipped by state mutations and explicit invalidate
//   • the parent-stack used while a view-fn runs (current open
//     ancestor chain)
//   • the persistent state-slot map keyed by call-site hash
//   • the per-rebuild click-handler map keyed by element id hash
//
// One ImmRuntime per Document, lazily created on first `set_imm_view`.
// Thread-local pointer `imm_active_runtime()` is set while a view fn
// runs so imm::* free functions can find it without an explicit
// context argument.

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

// Forward-declared lexbor types so this header doesn't leak the
// upstream API.
struct lxb_dom_node;
typedef struct lxb_dom_node lxb_dom_node_t;
struct lxb_dom_element;
typedef struct lxb_dom_element lxb_dom_element_t;
struct lxb_html_document;
typedef struct lxb_html_document lxb_html_document_t;

namespace affineui {
class Document;
}

namespace affineui::detail {

struct ImmStateSlot {
    void*       data{nullptr};
    std::size_t size{0};
    void (*dtor)(void*){nullptr};
};

class ImmRuntime {
public:
    ImmRuntime();
    ~ImmRuntime();

    ImmRuntime(const ImmRuntime&)            = delete;
    ImmRuntime& operator=(const ImmRuntime&) = delete;

    /// Bind to a freshly-parsed document. Called by Document::set_html
    /// after the lexbor doc has been reset; keeps the runtime in sync
    /// with whichever document its host is currently on.
    void bind(Document* owner, lxb_html_document_t* doc);

    void set_view_fn(std::function<void()> fn);
    bool dirty() const noexcept { return dirty_; }
    void mark_dirty()     noexcept { dirty_ = true; }
    void clear_dirty()    noexcept { dirty_ = false; }
    bool has_view_fn()    const noexcept { return static_cast<bool>(view_fn_); }

    /// Clear body's children, run the view function, leave the DOM in
    /// its new shape. The caller (Document::tick_imm) handles
    /// re-cascading + re-collecting blocks afterward.
    void run_view_fn();

    /// State-slot getter, called by imm::use_state<T>(...). Returns a
    /// stable pointer for the lifetime of this Document. On first
    /// access the slot is created and the user-supplied `ctor` runs
    /// once.
    void* get_or_create_slot(std::uint64_t key,
                             std::size_t   size,
                             std::size_t   align,
                             void (*ctor)(void*),
                             void (*dtor)(void*));

    /// Register a click handler against a hash-derived element id
    /// (the same hash the imm::* tag function used to stamp the
    /// element's `id` attribute).
    void register_click(std::uint64_t scope_hash, std::function<void()> cb);

    /// Look up + invoke a click handler by element id (e.g.
    /// "aui-imm-12345"). Returns whether a handler was found.
    bool invoke_click(std::string_view elem_id);

    // ── Used by imm::* during view-fn execution ─────────────────────
    //
    // None of these methods are valid outside a view-fn run.

    /// Create an element, set its `id` (always) + `class` (when non-
    /// empty), THEN insert into the parent. Order matters: lexbor's
    /// ev_insert hook runs the CSS cascade against the element on
    /// insert, and only sees attributes already present. Setting
    /// `class` after insert would miss class selectors. See lexbor's
    /// `examples/lexbor/styles/events_insert.c` for the canonical
    /// pattern.
    lxb_dom_element_t* open_element(std::string_view tag,
                                    std::uint64_t scope_hash,
                                    std::string_view classes = {});
    void close_element();
    void set_class(lxb_dom_element_t* el, std::string_view cls);
    void set_attr(lxb_dom_element_t* el,
                  std::string_view name,
                  std::string_view value);
    void append_text_to_current(std::string_view text);

private:
    Document*             owner_{nullptr};
    lxb_html_document_t*  doc_{nullptr};
    std::function<void()> view_fn_;
    bool                  dirty_{true};

    // Set-until-mismatch reconciler state. For each ancestor in
    // parent_stack_ we keep a cursor pointing at the next sibling we
    // expect to match against the next open_element / text call. A
    // match (same id-via-scope-hash, same tag, or same node type for
    // text) reuses the existing node and advances the cursor. A
    // mismatch destroys the cursor and everything after it in this
    // parent's child list, then inserts a fresh node.
    std::vector<lxb_dom_node_t*> parent_stack_;
    std::vector<lxb_dom_node_t*> cursor_stack_;

    // Keyed by call-site hash; survives re-renders.
    std::unordered_map<std::uint64_t, ImmStateSlot> state_slots_;
    // Keyed by scope-hash (matches the element id "aui-imm-{hash}");
    // cleared and rebuilt each run_view_fn.
    std::unordered_map<std::uint64_t, std::function<void()>> click_handlers_;
};

/// Thread-local pointer to the runtime currently running its view fn.
/// imm::* free functions use this to find their context. nullptr when
/// no view fn is active (calls outside a run_view_fn are no-ops).
ImmRuntime*& imm_active_runtime();

}  // namespace affineui::detail
