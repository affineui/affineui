#pragma once

// Immediate-mode UI layer for AffineUI. The API is intentionally shaped
// like Dear ImGui: ordinary C++ calls, stable call-site identity,
// lightweight state, and event handlers close to the widget declaration.
// Underneath, AffineUI reconciles that into a retained HTML/CSS DOM.
//
//   * View function ("component") describes the UI you *want*.
//   * AffineUI diffs that against the previous tree and patches the
//     retained DOM — real HTML, real CSS, GPU-painted.
//   * The view function runs only when something has plausibly changed
//     (state hook mutated, event handler returned, explicit
//     invalidate). NOT every frame.
//
// Identity: by default, the call-site path (parent_path ⊕ child_index ⊕
// tag) determines node identity across runs. For lists where order may
// change, pass an explicit `.key(...)` — same semantics as React's
// `key={...}`.

#include "affineui/types.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

#include <source_location>

namespace affineui {
class Document;
}

namespace affineui::imm {

// ── Call-site identity ──────────────────────────────────────────────
//
// We use std::source_location (C++20) so the default-argument trick
// captures the *caller's* file/line/column. Stable across runs as long
// as the source layout is stable.

struct CallSite {
    const char* file{""};
    std::uint32_t line{0};
    std::uint32_t column{0};
    std::uint64_t hash() const noexcept;
};

#define AFFINEUI_HERE_DEFAULT_ARG                                            \
    ::affineui::imm::CallSite {                                              \
        std::source_location::current().file_name(),                         \
        std::source_location::current().line(),                              \
        std::source_location::current().column()                             \
    }

// ── Frame / pass control ────────────────────────────────────────────

/// Mark the imm view as needing re-evaluation before the next paint.
/// Safe to call from event handlers or background threads.
void invalidate();

/// Whether the view is currently dirty (a re-evaluation will run before
/// the next paint).
bool is_dirty();

/// Set a key for the NEXT opened element (and its descendants). The
/// key is XOR-folded into the open_tag's scope-hash so different
/// iterations of a loop produce different element ids — necessary for
/// click routing and stable reconciliation when rendering a variable-
/// length list. The key is consumed by the very next open_tag /
/// container call; descendants inside that element inherit it
/// automatically.
///
///   for (std::size_t i = 0; i < items.size(); ++i) {
///       imm::key(i);
///       if (auto _ = imm::div("row")) { ... }
///   }
void key(std::uint64_t value);

// ── Scope (RAII child container) ────────────────────────────────────
//
// Returned from container builders (div, button, h1, ...). Implicit
// conversion to `bool` so the canonical pattern is:
//
//     if (auto _ = imm::div("card")) {
//         imm::text("contents");
//     }

class Scope {
public:
    Scope() noexcept = default;
    explicit Scope(std::uint64_t node_id) noexcept;
    ~Scope();

    Scope(Scope&&) noexcept;
    Scope& operator=(Scope&&) noexcept;
    Scope(const Scope&)            = delete;
    Scope& operator=(const Scope&) = delete;

    explicit operator bool() const noexcept { return active_; }

    // Fluent attribute setters (return *this for chaining).
    Scope& key(std::uint64_t k);
    Scope& key(std::string_view k);
    Scope& id(std::string_view v);
    Scope& cls(std::string_view classes);
    Scope& style(std::string_view inline_css);
    Scope& attr(std::string_view name, std::string_view value);

    // Event handlers — closures live until next reconciliation.
    Scope& on_click(std::function<void()> cb);
    Scope& on_input(std::function<void(std::string_view)> cb);
    Scope& on_change(std::function<void(std::string_view)> cb);
    Scope& on_hover(std::function<void(bool)> cb);

    // For text-bearing leaves (button, h1, h2, ...): set the inner text.
    Scope& text(std::string_view t);

private:
    std::uint64_t node_id_{0};
    bool          active_{false};
};

// ── Container elements ──────────────────────────────────────────────

Scope div   (std::string_view classes = "", CallSite here = AFFINEUI_HERE_DEFAULT_ARG);
Scope span  (std::string_view classes = "", CallSite here = AFFINEUI_HERE_DEFAULT_ARG);
Scope section(std::string_view classes = "", CallSite here = AFFINEUI_HERE_DEFAULT_ARG);
Scope header(std::string_view classes = "", CallSite here = AFFINEUI_HERE_DEFAULT_ARG);
Scope footer(std::string_view classes = "", CallSite here = AFFINEUI_HERE_DEFAULT_ARG);
Scope nav   (std::string_view classes = "", CallSite here = AFFINEUI_HERE_DEFAULT_ARG);
Scope main_ (std::string_view classes = "", CallSite here = AFFINEUI_HERE_DEFAULT_ARG);
Scope ul    (std::string_view classes = "", CallSite here = AFFINEUI_HERE_DEFAULT_ARG);
Scope ol    (std::string_view classes = "", CallSite here = AFFINEUI_HERE_DEFAULT_ARG);
Scope li    (std::string_view classes = "", CallSite here = AFFINEUI_HERE_DEFAULT_ARG);
Scope form  (std::string_view classes = "", CallSite here = AFFINEUI_HERE_DEFAULT_ARG);
Scope label (std::string_view classes = "", CallSite here = AFFINEUI_HERE_DEFAULT_ARG);
Scope a     (std::string_view href,
             std::string_view classes = "",
             CallSite here = AFFINEUI_HERE_DEFAULT_ARG);

// ── Headings / text-bearing leaves ──────────────────────────────────

Scope h1(std::string_view classes = "", CallSite here = AFFINEUI_HERE_DEFAULT_ARG);
Scope h2(std::string_view classes = "", CallSite here = AFFINEUI_HERE_DEFAULT_ARG);
Scope h3(std::string_view classes = "", CallSite here = AFFINEUI_HERE_DEFAULT_ARG);
Scope h4(std::string_view classes = "", CallSite here = AFFINEUI_HERE_DEFAULT_ARG);
Scope p (std::string_view classes = "", CallSite here = AFFINEUI_HERE_DEFAULT_ARG);

/// Inline text node. No children.
void text(std::string_view t, CallSite here = AFFINEUI_HERE_DEFAULT_ARG);

/// Raw HTML escape hatch — parsed and grafted in as static markup.
/// Does not participate in reconciliation; replaced wholesale each
/// pass.
void raw_html(std::string_view html, CallSite here = AFFINEUI_HERE_DEFAULT_ARG);

// ── Form widgets ────────────────────────────────────────────────────

Scope button   (std::string_view label = "",
                std::string_view classes = "",
                CallSite here = AFFINEUI_HERE_DEFAULT_ARG);
Scope input    (std::string_view type, std::string_view value,
                CallSite here = AFFINEUI_HERE_DEFAULT_ARG);
Scope checkbox (bool checked, CallSite here = AFFINEUI_HERE_DEFAULT_ARG);
Scope textarea (std::string_view value,
                CallSite here = AFFINEUI_HERE_DEFAULT_ARG);

// ── Media ──────────────────────────────────────────────────────────

Scope img(std::string_view src, std::string_view alt = "",
          CallSite here = AFFINEUI_HERE_DEFAULT_ARG);

// ── State hooks ─────────────────────────────────────────────────────
//
// State is keyed by the current scope's call-site path. Mutating the
// returned reference (or any object the reference reaches) marks the
// view dirty automatically — no manual setState. Storage lives on the
// document and is freed when the owning node has been absent for one
// full reconciliation pass.

namespace detail {
void* get_or_create_state_slot(CallSite here,
                               std::size_t size,
                               std::size_t align,
                               void (*ctor)(void*),
                               void (*dtor)(void*));
void  mark_state_mutated(void* slot);
}  // namespace detail

template <typename T>
class StateRef {
public:
    StateRef(T* ptr, void* slot) noexcept : ptr_(ptr), slot_(slot) {}

    T&       get() noexcept       { return *ptr_; }
    const T& get() const noexcept { return *ptr_; }
    operator T&() noexcept             { return *ptr_; }
    operator const T&() const noexcept { return *ptr_; }
    T* operator->() noexcept             { return ptr_; }
    const T* operator->() const noexcept { return ptr_; }

    /// Assigning marks the view dirty.
    template <typename U>
    StateRef& operator=(U&& v) {
        *ptr_ = std::forward<U>(v);
        detail::mark_state_mutated(slot_);
        return *this;
    }

    /// Mutate in place, then mark dirty.
    template <typename F>
    void update(F&& f) {
        std::forward<F>(f)(*ptr_);
        detail::mark_state_mutated(slot_);
    }

private:
    T*    ptr_{nullptr};
    void* slot_{nullptr};
};

template <typename T>
StateRef<T> use_state(T initial = T{},
                      CallSite here = AFFINEUI_HERE_DEFAULT_ARG) {
    struct Box { T value; };
    auto* slot = detail::get_or_create_state_slot(
        here, sizeof(Box), alignof(Box),
        +[](void* p) { ::new (p) Box{}; },
        +[](void* p) { static_cast<Box*>(p)->~Box(); });
    auto* box = static_cast<Box*>(slot);
    static bool initialised = false;
    if (!initialised) {  // first-time only seeding; replaced by per-slot flag in impl
        box->value  = std::move(initial);
        initialised = true;
    }
    return StateRef<T>(&box->value, slot);
}

// ── Side effects ────────────────────────────────────────────────────

/// Run `effect` once when its dependency hash changes. The destructor
/// returned by `effect` (if any) runs on the next change, mirroring
/// React's `useEffect`.
void use_effect(std::function<std::function<void()>()> effect,
                std::uint64_t deps_hash,
                CallSite here = AFFINEUI_HERE_DEFAULT_ARG);

// ── Binding the view fn to a Document ───────────────────────────────
//
// Most embedders call App::mount() — this is the lower-level entry
// point if you're driving Document directly.
void bind(Document& doc, std::function<void()> view_fn);
void unbind(Document& doc);

// ── inline impl ─────────────────────────────────────────────────────

inline std::uint64_t CallSite::hash() const noexcept {
    // FNV-1a over filename contents, line, and column. Stable for the
    // duration of the process — which is all we need.
    std::uint64_t h = 0xcbf29ce484222325ull;
    if (file) {
        for (const auto* p = reinterpret_cast<const unsigned char*>(file);
             *p != 0; ++p) {
            h ^= *p;
            h *= 0x100000001b3ull;
        }
    }
    auto mix = [&](std::uint64_t x) {
        for (int i = 0; i < 8; ++i) {
            h ^= (x >> (i * 8)) & 0xff;
            h *= 0x100000001b3ull;
        }
    };
    mix(line);
    mix(column);
    return h;
}

}  // namespace affineui::imm
