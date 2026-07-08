// Public imm:: surface — thin wrappers over `detail::ImmRuntime`. The
// runtime owns the DOM-mutation work; this file just routes calls.
//
// Scope RAII: opening any container tag (`imm::div`, `imm::h1`,
// `imm::button`, ...) calls `runtime->open_element` which pushes onto
// the runtime's parent stack. The returned `Scope` is active; its
// destructor calls `runtime->close_element` which pops. Moves
// transfer the active bit so closing happens at most once.

#include "affineui/imm.h"
#include "framework/imm/imm_runtime.h"

#include <cstdint>
#include <utility>
#include <vector>

namespace affineui::imm {

using affineui::detail::ImmRuntime;
using affineui::detail::imm_active_runtime;

namespace {

ImmRuntime* rt() { return imm_active_runtime(); }

// Pending per-call key set by imm::key(v); consumed by the next
// open_tag. Combined with the inherited-key chain so all descendants
// of a keyed element automatically pick up the key without the user
// having to re-call imm::key inside each child.
thread_local std::uint64_t g_pending_key{0};
thread_local bool          g_has_pending_key{false};

// Cumulative XOR of keys from the chain of currently-open ancestors.
// Pushed by open_tag, popped by Scope's dtor / move. Empty stack
// implies an inherited key of 0 (no keying).
thread_local std::vector<std::uint64_t> g_inherited_key_stack;

std::uint64_t current_inherited_key() {
    return g_inherited_key_stack.empty() ? 0 : g_inherited_key_stack.back();
}

// Open a tag and return a constructed Scope. The Scope is always
// "active" (truthy) so the `if (auto s = imm::div(...))` idiom works
// even outside a view-fn run — useful for tests + driverless static
// analysis. When a runtime is bound, also mutates the DOM and class
// attribute; without a runtime, the scope is a no-op container.
Scope open_tag(std::string_view tag,
               std::string_view classes,
               CallSite here) {
    // Fold the pending key (from imm::key) into the inherited chain
    // for THIS element and all of its descendants.
    std::uint64_t inherited = current_inherited_key();
    if (g_has_pending_key) {
        inherited ^= g_pending_key;
        g_pending_key = 0;
        g_has_pending_key = false;
    }
    const std::uint64_t hash = here.hash() ^ inherited;
    if (auto* r = rt()) {
        // Pass class through so the runtime can set it BEFORE insert.
        // Lexbor's cascade runs in the ev_insert hook against whatever
        // attributes the element has at insert time — setting class
        // afterwards would miss `.cls` selector matches.
        r->open_element(tag, hash, classes);
    }
    g_inherited_key_stack.push_back(inherited);
    return Scope{hash};
}

}  // namespace

void key(std::uint64_t value) {
    g_pending_key = value;
    g_has_pending_key = true;
}

void invalidate() {
    if (auto* r = rt()) r->mark_dirty();
}

bool is_dirty() {
    if (auto* r = rt()) return r->dirty();
    return false;
}

// ── Scope ───────────────────────────────────────────────────────────

Scope::Scope(std::uint64_t node_id) noexcept
    : node_id_{node_id}, active_{true} {}

Scope::~Scope() {
    if (!active_) return;
    if (!g_inherited_key_stack.empty()) g_inherited_key_stack.pop_back();
    if (auto* r = rt()) r->close_element();
}

Scope::Scope(Scope&& o) noexcept : node_id_{o.node_id_}, active_{o.active_} {
    o.active_ = false;
}
Scope& Scope::operator=(Scope&& o) noexcept {
    if (this != &o) {
        if (active_) {
            if (!g_inherited_key_stack.empty()) g_inherited_key_stack.pop_back();
            if (auto* r = rt()) r->close_element();
        }
        node_id_  = o.node_id_;
        active_   = o.active_;
        o.active_ = false;
    }
    return *this;
}

Scope& Scope::key(std::uint64_t)       { return *this; }   // Phase 3 (loops + reorder)
Scope& Scope::key(std::string_view)    { return *this; }
Scope& Scope::id(std::string_view)     { return *this; }   // overrides auto-id; Phase 3
Scope& Scope::cls(std::string_view)    { return *this; }
Scope& Scope::style(std::string_view)  { return *this; }
Scope& Scope::attr(std::string_view, std::string_view) { return *this; }

Scope& Scope::on_click(std::function<void()> cb) {
    if (active_) {
        if (auto* r = rt()) r->register_click(node_id_, std::move(cb));
    }
    return *this;
}
Scope& Scope::on_input(std::function<void(std::string_view)>)       { return *this; }
Scope& Scope::on_change(std::function<void(std::string_view)>)      { return *this; }
Scope& Scope::on_hover(std::function<void(bool)>)                   { return *this; }

Scope& Scope::text(std::string_view t) {
    if (active_) {
        if (auto* r = rt()) r->append_text_to_current(t);
    }
    return *this;
}

// ── Container / leaf builders ───────────────────────────────────────

Scope div    (std::string_view c, CallSite h) { return open_tag("div",     c, h); }
Scope span   (std::string_view c, CallSite h) { return open_tag("span",    c, h); }
Scope section(std::string_view c, CallSite h) { return open_tag("section", c, h); }
Scope header (std::string_view c, CallSite h) { return open_tag("header",  c, h); }
Scope footer (std::string_view c, CallSite h) { return open_tag("footer",  c, h); }
Scope nav    (std::string_view c, CallSite h) { return open_tag("nav",     c, h); }
Scope main_  (std::string_view c, CallSite h) { return open_tag("main",    c, h); }
Scope ul     (std::string_view c, CallSite h) { return open_tag("ul",      c, h); }
Scope ol     (std::string_view c, CallSite h) { return open_tag("ol",      c, h); }
Scope li     (std::string_view c, CallSite h) { return open_tag("li",      c, h); }
Scope form   (std::string_view c, CallSite h) { return open_tag("form",    c, h); }
Scope label  (std::string_view c, CallSite h) { return open_tag("label",   c, h); }
Scope a      (std::string_view /*href*/, std::string_view c, CallSite h) {
    // href setting needs an element handle — open_tag doesn't return
    // one. Phase 3 will plumb the lxb_dom_element_t* back so attr
    // setters work. For now anchor renders without the link.
    return open_tag("a", c, h);
}

Scope h1(std::string_view c, CallSite h) { return open_tag("h1", c, h); }
Scope h2(std::string_view c, CallSite h) { return open_tag("h2", c, h); }
Scope h3(std::string_view c, CallSite h) { return open_tag("h3", c, h); }
Scope h4(std::string_view c, CallSite h) { return open_tag("h4", c, h); }
Scope p (std::string_view c, CallSite h) { return open_tag("p",  c, h); }

void text(std::string_view t, CallSite /*here*/) {
    if (auto* r = rt()) r->append_text_to_current(t);
}

void raw_html(std::string_view, CallSite) {}     // Phase 3 — needs lexbor parse

Scope button  (std::string_view label, std::string_view classes, CallSite h) {
    auto s = open_tag("button", classes, h);
    if (s && !label.empty()) {
        if (auto* r = rt()) r->append_text_to_current(label);
    }
    return s;
}
Scope input   (std::string_view, std::string_view, CallSite h) { return open_tag("input", {}, h); }
Scope checkbox(bool, CallSite h)                               { return open_tag("input", {}, h); }
Scope textarea(std::string_view, CallSite h)                   { return open_tag("textarea", {}, h); }

Scope img(std::string_view, std::string_view, CallSite h) { return open_tag("img", {}, h); }

void use_effect(std::function<std::function<void()>()>, std::uint64_t, CallSite) {}

void bind(Document&, std::function<void()>)   {}   // Ui::mount supersedes this entry point
void unbind(Document&)                        {}

namespace detail {

void* get_or_create_state_slot(CallSite here,
                               std::size_t size,
                               std::size_t align,
                               void (*ctor)(void*),
                               void (*dtor)(void*)) {
    auto* r = rt();
    if (!r) {
        // No active runtime (used outside a view-fn run) — return a
        // leaked one-shot slot so the calling use_state<T> still
        // compiles + returns a valid ref. Not the production path.
        void* p = ::operator new(size);
        if (ctor) ctor(p);
        (void)align; (void)dtor;
        return p;
    }
    return r->get_or_create_slot(here.hash(), size, align, ctor, dtor);
}

void mark_state_mutated(void* /*slot*/) {
    // Phase 3 refinement: track Document* per slot so cross-thread /
    // cross-Document state mutation invalidates the *right* runtime.
    // Today: the runtime that's bound to the active Ui dispatches the
    // event handler that called us, so the active-runtime pointer is
    // correct on this thread.
    if (auto* r = rt()) r->mark_dirty();
}

}  // namespace detail

}  // namespace affineui::imm
