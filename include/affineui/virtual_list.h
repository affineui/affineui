#pragma once

// Virtual list & tree providers for AffineUI.
//
// A virtual list renders a window of only the rows that are on screen (plus a
// small overscan) while presenting a full-height scroll extent, so a list of
// hundreds of thousands of items scrolls at display refresh with flat memory
// and behaves — scrollbar, wheel, keyboard, selection, drag — exactly as if
// every row were really rendered.
//
// The provider is a *stateless bridge*, not a model. It stores no items, no
// selection, and no expansion state: the data lives in the app's own model,
// and the provider is a set of guarded callbacks (see callback.h) that the
// virtual-list widget invokes to (a) ask questions about that data — how many
// rows, how tall is row i, is row i selected — and (b) *build the line item*
// the list displays for row i (often a single `label`, sometimes richer
// content, sometimes just text). Every callback re-derives from the source of
// truth on each call, which is exactly why recycling is safe: there is no
// provider-side state to go stale.
//
// Ownership: a panel controller typically owns its provider via a unique_ptr.
// The widget, however, holds only a WeakRef<VirtualListProvider>, so a list
// that outlives (or is wrongly shared past) its provider renders empty rather
// than crashing — the same hard-to-crash contract the rest of AffineUI honours.
//
// Providers are populated fluently and carry guarded callbacks so they bind
// cleanly from every language (Python callables, C#/Rust delegates over the C
// ABI) — there is deliberately no virtual-method base class to override, which
// would not survive the C ABI boundary.
//
//   struct Outliner : affineui::Trackable {           // the panel controller
//       std::vector<Node> nodes;
//       Selection selection;
//       std::unique_ptr<affineui::VirtualListProvider> rows;
//
//       Outliner() {
//           rows = std::make_unique<affineui::VirtualListProvider>();
//           rows->on_item_count([this] { return nodes.size(); })
//               .on_item_text([this](std::size_t i) { return nodes[i].name; })
//               .on_is_selected([this](std::size_t i) {
//                   return selection.contains(nodes[i].id); })
//               .on_activate([this](std::size_t i, affineui::SelectMod m) {
//                   selection.apply(m, nodes[i].id); })
//               .on_drop([this](std::size_t src, std::size_t dst,
//                               affineui::DropPos p) { move_node(src, dst, p); });
//       }
//   };
//
//   // in the view build:
//   view.virtual_list("outliner", *outliner.rows);

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_set>
#include <vector>

#include "affineui/weak_ref.h"

namespace affineui {

class View;  // build_item renders into a View; defined in view.h

/// The resolved window of a virtual list for a given scroll position: which
/// items to build and where to place them. `first` is the first item touching
/// the top/left edge of the viewport; `[begin, end)` is the item range actually
/// built (window + overscan); `lead_px` is the leading-spacer size (the whole
/// items before `begin`) and `trail_px` is the trailing-spacer size, so lead +
/// built rows + trail = the full extent. (virtual_offset / virtual_item_at /
/// compute_window are defined as templates after the provider classes below.)
struct VirtualWindow {
    std::size_t  begin{0};
    std::size_t  end{0};
    std::size_t  first{0};
    double       lead_px{0.0};
    double       trail_px{0.0};
    double       total_px{0.0};
};

/// Selection intent carried by an activation (click). The widget derives this
/// from the modifier keys held during the click and hands it to on_activate;
/// the app model decides what it means (single-select, toggle, extend range).
enum class SelectMod {
    Replace,  ///< plain click — select only this row
    Toggle,   ///< ctrl/cmd-click — toggle this row in/out of the selection
    Range,    ///< shift-click — extend the selection from the anchor to this row
};

/// Where a drop lands relative to the target row.
enum class DropPos {
    Before,  ///< insert before the target row
    Into,    ///< drop into the target row (e.g. reparent under a tree node)
    After,   ///< insert after the target row
};

/// An optional ready-made selection model over logical row indices, with the
/// standard replace / toggle / range semantics and an anchor held as an index
/// (never a DOM pointer — so it survives row recycling). Plug it into a
/// provider in one line:
///
///   sel_.on_change([this] { app_.rebuild_view(); });
///   provider.on_is_selected([this](std::size_t i) { return sel_.contains(i); })
///           .on_activate([this](std::size_t i, affineui::SelectMod m) {
///               sel_.apply(i, m); });
///
/// Apps that track selection by id or need custom rules can ignore this and
/// implement on_activate/on_is_selected directly.
class IndexSelection {
public:
    using ChangedFn = std::function<void()>;

    /// Apply a click on row `i` with the given modifier intent. `count` (when
    /// provided > 0) clamps the range so an out-of-date anchor can't select past
    /// the end after items were removed.
    void apply(std::size_t i, SelectMod mod, std::size_t count = 0) {
        switch (mod) {
            case SelectMod::Toggle: {
                if (auto it = find(i); it != selected_.end()) selected_.erase(it);
                else selected_.push_back(i);
                anchor_ = i;
                break;
            }
            case SelectMod::Range: {
                std::size_t lo = std::min(anchor_, i);
                std::size_t hi = std::max(anchor_, i);
                if (count > 0 && hi >= count) hi = count - 1;
                selected_.clear();
                for (std::size_t j = lo; j <= hi; ++j) selected_.push_back(j);
                // anchor stays put so a further shift-click re-ranges from it
                break;
            }
            case SelectMod::Replace:
            default:
                selected_.assign(1, i);
                anchor_ = i;
                break;
        }
        notify();
    }

    void clear() { selected_.clear(); notify(); }

    [[nodiscard]] bool contains(std::size_t i) const {
        return find(i) != selected_.end();
    }
    [[nodiscard]] bool empty() const noexcept { return selected_.empty(); }
    [[nodiscard]] std::size_t size() const noexcept { return selected_.size(); }
    [[nodiscard]] std::size_t anchor() const noexcept { return anchor_; }
    [[nodiscard]] const std::vector<std::size_t>& indices() const noexcept {
        return selected_;
    }

    void on_change(ChangedFn fn) { changed_ = std::move(fn); }

private:
    [[nodiscard]] std::vector<std::size_t>::const_iterator find(
        std::size_t i) const {
        return std::find(selected_.begin(), selected_.end(), i);
    }
    std::vector<std::size_t>::iterator find(std::size_t i) {
        return std::find(selected_.begin(), selected_.end(), i);
    }
    void notify() { if (changed_) changed_(); }

    std::vector<std::size_t> selected_;
    std::size_t              anchor_{0};
    ChangedFn                changed_;
};

/// A stateless bridge between an app-owned list model and a virtual-list
/// widget. Holds only guarded callbacks; stores no data. Trackable so the
/// widget can hold a WeakRef to it. Populated fluently — every setter returns
/// the most-derived provider type (via CRTP) so chaining stays typed even when
/// a base setter precedes a derived one on VirtualTreeProvider.
///
/// `Self` is the concrete provider type; the fluent setters return `Self&`.
template <typename Self>
class VirtualListProviderBase : public Trackable {
public:
    // Callback signatures. The provider itself is the crash guard: the widget
    // holds a WeakRef and locks it before invoking any of these, so a destroyed
    // provider degrades to an empty list rather than a dangling call. Callbacks
    // are plain std::function (not the bound Callback form) so they bind
    // uniformly from every language; the controller that owns the provider via
    // unique_ptr is destroyed with it, keeping the captured `this` valid for the
    // provider's whole lifetime.
    using ItemCountFn = std::function<std::size_t()>;
    using ItemSizeFn  = std::function<double(std::size_t)>;
    using BuildItemFn = std::function<void(View&, std::size_t)>;
    using ItemTextFn  = std::function<std::string(std::size_t)>;
    using IsSelFn     = std::function<bool(std::size_t)>;
    using ActivateFn  = std::function<void(std::size_t, SelectMod)>;
    using DropFn      = std::function<void(std::size_t, std::size_t, DropPos)>;
    using SetCheckedFn = std::function<void(std::size_t, bool)>;

    VirtualListProviderBase() = default;

    /// Number of rows in the list (data-that-lives-elsewhere). Required.
    Self& on_item_count(ItemCountFn fn) {
        item_count_ = std::move(fn);
        return self();
    }

    /// Height of row i in px. Optional; omit for a uniform row height (see
    /// default_item_size). Enables variable-height rows.
    Self& on_item_size(ItemSizeFn fn) {
        item_size_ = std::move(fn);
        return self();
    }

    /// Build the line item for row i into the View. Takes precedence over
    /// on_item_text when both are set. Rows must be *structurally uniform*
    /// (same widget shape for every row, varying only text/attributes) so the
    /// reconciler recycles the row DOM instead of rebuilding it.
    Self& on_build_item(BuildItemFn fn) {
        build_item_ = std::move(fn);
        return self();
    }

    /// Convenience for pure-text rows: return the text for row i and the widget
    /// wraps it in a default label row. Ignored if on_build_item is set.
    Self& on_item_text(ItemTextFn fn) {
        item_text_ = std::move(fn);
        return self();
    }

    /// Whether row i is selected. Re-asked on every rebuild — selection lives
    /// in the app model, never in the (recycled) DOM. Optional.
    Self& on_is_selected(IsSelFn fn) {
        is_selected_ = std::move(fn);
        return self();
    }

    /// A row was activated (clicked). SelectMod carries the modifier intent;
    /// the app model updates its selection. Optional.
    Self& on_activate(ActivateFn fn) {
        activate_ = std::move(fn);
        return self();
    }

    /// A row was dropped onto another. Reported by logical index; the app model
    /// performs the reorder/reparent. Optional (enables row drag-and-drop).
    Self& on_drop(DropFn fn) {
        drop_ = std::move(fn);
        return self();
    }

    // --- Checkbox mode ----------------------------------------------------
    // Rows carry TWO independent states: selected and checked. When checkbox
    // mode is on, every row renders a leading checkbox (structurally uniform,
    // so rows keep recycling); a checkbox press toggles checked WITHOUT
    // selecting the row. Checked state, like selection, lives in the app
    // model — keyed by a stable identity (handle for trees), never the DOM.

    /// Show a checkbox on every row. Togglable at runtime (mode flips are one
    /// structural rebuild; scrolling stays attribute-only either way).
    Self& checkboxes(bool on) {
        checkboxes_ = on;
        return self();
    }
    /// Whether row i is checked. Re-asked on every rebuild.
    Self& on_is_checked(IsSelFn fn) {
        is_checked_ = std::move(fn);
        return self();
    }
    /// Row i's checkbox was toggled to `on`. The app model updates.
    Self& on_set_checked(SetCheckedFn fn) {
        set_checked_ = std::move(fn);
        return self();
    }

    /// Uniform row height used when on_item_size is not set (px).
    Self& default_item_size(double px) {
        default_item_size_ = px > 0.0 ? px : default_item_size_;
        return self();
    }

    // --- Queries the widget uses (all null-safe) -------------------------

    [[nodiscard]] std::size_t item_count() const {
        return item_count_ ? item_count_() : 0;
    }
    [[nodiscard]] double item_size(std::size_t i) const {
        if (item_size_) {
            const double h = item_size_(i);
            return h > 0.0 ? h : default_item_size_;
        }
        return default_item_size_;
    }
    [[nodiscard]] bool has_variable_sizes() const {
        return static_cast<bool>(item_size_);
    }
    [[nodiscard]] double default_size() const { return default_item_size_; }
    [[nodiscard]] bool is_selected(std::size_t i) const {
        return is_selected_ && is_selected_(i);
    }
    [[nodiscard]] const BuildItemFn& build_item() const { return build_item_; }
    [[nodiscard]] const ItemTextFn& item_text() const { return item_text_; }

    void activate(std::size_t i, SelectMod mod) const {
        if (activate_) activate_(i, mod);
    }
    void drop(std::size_t src, std::size_t dst, DropPos pos) const {
        if (drop_) drop_(src, dst, pos);
    }
    [[nodiscard]] bool has_checkboxes() const noexcept { return checkboxes_; }
    [[nodiscard]] bool is_checked(std::size_t i) const {
        return is_checked_ && is_checked_(i);
    }
    void set_checked(std::size_t i, bool on) const {
        if (set_checked_) set_checked_(i, on);
    }

private:
    Self& self() { return static_cast<Self&>(*this); }

    ItemCountFn item_count_;
    ItemSizeFn  item_size_;
    BuildItemFn build_item_;
    ItemTextFn  item_text_;
    IsSelFn     is_selected_;
    ActivateFn  activate_;
    DropFn      drop_;
    IsSelFn     is_checked_;
    SetCheckedFn set_checked_;
    bool        checkboxes_{false};
    double      default_item_size_{24.0};
};

/// The concrete list provider: a plain virtual list with no tree shape.
class VirtualListProvider
    : public VirtualListProviderBase<VirtualListProvider> {};

/// A virtual tree, expressed as a virtual list over the *flattened, currently-
/// expanded* nodes: a tree is a list whose index resets when nodes open or
/// close. The provider answers item_count/item_size/build over that flattened
/// view (row i is the i-th visible node) and adds the tree-shape questions:
/// depth, whether a row can expand, whether it is expanded, and a toggle hook.
///
/// Because the list is the flattened-expanded view, expand/collapse is just a
/// change in item_count() plus a re-window: the app toggles its model in
/// on_toggle and the next build reflects the new flattened set. Chevron and
/// icon slots are always present (hidden when not applicable) so depth/expand
/// changes are attribute-only and rows stay structurally uniform for recycling.
class VirtualTreeProvider
    : public VirtualListProviderBase<VirtualTreeProvider> {
public:
    using DepthFn      = std::function<int(std::size_t)>;
    using ExpandableFn = std::function<bool(std::size_t)>;
    using ExpandedFn   = std::function<bool(std::size_t)>;
    using ToggleFn     = std::function<void(std::size_t)>;

    VirtualTreeProvider() = default;

    /// Indent depth of the flattened row i (0 = root level). Required.
    VirtualTreeProvider& on_depth(DepthFn fn) {
        depth_ = std::move(fn);
        return *this;
    }

    /// Whether row i has children (shows a chevron). Optional.
    VirtualTreeProvider& on_is_expandable(ExpandableFn fn) {
        expandable_ = std::move(fn);
        return *this;
    }

    /// Whether row i is currently expanded (chevron open). Optional.
    VirtualTreeProvider& on_is_expanded(ExpandedFn fn) {
        expanded_ = std::move(fn);
        return *this;
    }

    /// Row i's chevron was clicked. The app flips its model's expansion state;
    /// the next build reflects the new flattened set. Optional.
    VirtualTreeProvider& on_toggle(ToggleFn fn) {
        toggle_ = std::move(fn);
        return *this;
    }

    [[nodiscard]] int depth(std::size_t i) const {
        return depth_ ? depth_(i) : 0;
    }
    [[nodiscard]] bool is_expandable(std::size_t i) const {
        return expandable_ && expandable_(i);
    }
    [[nodiscard]] bool is_expanded(std::size_t i) const {
        return expanded_ && expanded_(i);
    }
    void toggle(std::size_t i) const {
        if (toggle_) toggle_(i);
    }

private:
    DepthFn      depth_;
    ExpandableFn expandable_;
    ExpandedFn   expanded_;
    ToggleFn     toggle_;
};

/// Turns an opaque, app-owned tree into the flattened list of *visible* nodes a
/// VirtualTreeProvider needs, and wires that provider — so an app can show a
/// tree without hand-rolling the flatten + expand/collapse bookkeeping.
///
/// The tree data lives in some app object of type `Data`, held here by WeakRef:
/// if it is destroyed the flattened list empties and the tree renders blank
/// rather than walking freed memory. Nodes are identified by an opaque `Handle`
/// — an 8-byte value type the flattener only ever *stores*, never dereferences.
/// The item pointer used to draw a row is resolved from the handle transiently
/// at render time, so it can never dangle. A `Handle` can be any of:
///   • a raw pointer      — static data that never moves/frees (resolve = cast)
///   • a WeakRef<Item>     — dynamic data; resolve = handle.lock() (self-guards)
///   • a uint64 / id       — app-indexed; resolve looks it up
///   • a key into a map    — resolve = data->map.find(key)
/// All are the same size (~8 bytes). The app supplies the walk (roots, children,
/// label, has-children) and a resolver taking the locked `Data*`; the adapter
/// owns the set of expanded handles and rebuilds the flattened view on toggle.
///
///   TreeFlattener<Scene> flat{scene_weak};
///   flat.on_roots([](Scene* s, std::vector<Node>& out){ s->roots(out); })
///       .on_children([](Scene* s, Node n, std::vector<Node>& out){ ... })
///       .on_label([](Scene* s, Node n){ return s->name(n); })
///       .on_has_children([](Scene* s, Node n){ return s->child_count(n) > 0; });
///   flat.wire(tree_provider);            // provider now reflects the tree
///   flat.on_changed([this]{ app_.rebuild_view(); });
template <typename Data, typename Item = void, typename Handle = std::uintptr_t>
class TreeFlattener {
public:
    using RootsFn       = std::function<void(Data*, std::vector<Handle>&)>;
    using ChildrenFn    = std::function<void(Data*, Handle, std::vector<Handle>&)>;
    using LabelFn       = std::function<std::string(Data*, Handle)>;
    using HasChildrenFn = std::function<bool(Data*, Handle)>;
    // Resolve a node handle to the actual item pointer — "the thing you need to
    // draw." Called at render time so the row builder draws from the live item.
    using ResolveFn     = std::function<Item*(Data*, Handle)>;
    using RenderFn      = std::function<void(View&, Item*)>;
    using ChangedFn     = std::function<void()>;

    explicit TreeFlattener(WeakRef<Data> data) : data_(data) {}

    TreeFlattener& on_roots(RootsFn fn) { roots_ = std::move(fn); return *this; }
    TreeFlattener& on_children(ChildrenFn fn) {
        children_ = std::move(fn); return *this;
    }
    TreeFlattener& on_label(LabelFn fn) { label_ = std::move(fn); return *this; }
    TreeFlattener& on_has_children(HasChildrenFn fn) {
        has_children_ = std::move(fn); return *this;
    }
    /// Resolve a handle to its item pointer (used at render). Optional; pair with
    /// on_render to draw a rich row from the item.
    TreeFlattener& on_resolve(ResolveFn fn) { resolve_ = std::move(fn); return *this; }
    /// Draw a row from the resolved item pointer. Optional; if unset, rows show
    /// on_label text. Takes precedence over on_label when both are set.
    TreeFlattener& on_render(RenderFn fn) { render_ = std::move(fn); return *this; }
    TreeFlattener& on_changed(ChangedFn fn) {
        changed_ = std::move(fn); return *this;
    }

    /// Resolve the flattened row i to its live item pointer (locked from the
    /// weak-ref'd data). Returns null if the data is gone or i is out of range.
    [[nodiscard]] Item* item_at(std::size_t i) const {
        Data* d = data_.lock();
        if (!d || !resolve_ || i >= flat_.size()) return nullptr;
        return resolve_(d, flat_[i].handle);
    }
    /// Resolve an arbitrary handle to its item pointer.
    [[nodiscard]] Item* resolve(Handle h) const {
        Data* d = data_.lock();
        return (d && resolve_) ? resolve_(d, h) : nullptr;
    }

    /// Expand / collapse by handle (app-facing; e.g. to expand-all or restore a
    /// saved state). Re-flattens and notifies.
    void set_expanded(Handle h, bool open) {
        const bool changed = open ? expanded_.insert(h).second
                                  : (expanded_.erase(h) != 0);
        if (changed) rebuild();
    }
    [[nodiscard]] bool is_expanded(Handle h) const {
        return expanded_.count(h) != 0;
    }

    // ── Selection (HANDLE-based) ─────────────────────────────────────────
    // Tree selection is keyed by handle, never by flattened index: opening or
    // closing a node renumbers every row below it, so an index-keyed selection
    // silently migrates onto different items. Handles are stable identities.
    //
    // CONTRACT: a handle must be UNIQUE TO THE ITEM for its lifetime — a
    // monotonic id, a WeakRef, or a stable pointer. Never recycle a handle
    // onto a different item (a reused slot index, say), or the selection and
    // expansion recorded against the old item silently apply to the new one.

    /// Row activation (plain / Ctrl / Shift click), by flattened row index.
    /// Wired automatically by wire(); also callable by the app. Range selects
    /// the handles currently visible between the anchor and the clicked row.
    void activate(std::size_t i, SelectMod mod) {
        if (i >= flat_.size()) return;
        const Handle h = flat_[i].handle;
        switch (mod) {
            case SelectMod::Toggle:
                if (!selected_.erase(h)) selected_.insert(h);
                sel_anchor_ = h;
                has_sel_anchor_ = true;
                break;
            case SelectMod::Range: {
                std::size_t a = i;
                if (has_sel_anchor_) {
                    const std::size_t found = index_of(sel_anchor_);
                    if (found != npos) a = found;
                }
                const auto lo = std::min(a, i);
                const auto hi = std::max(a, i);
                selected_.clear();
                for (std::size_t k = lo; k <= hi; ++k) {
                    selected_.insert(flat_[k].handle);
                }
                if (!has_sel_anchor_) {
                    sel_anchor_ = h;
                    has_sel_anchor_ = true;
                }
                break;
            }
            case SelectMod::Replace:
            default:
                selected_.clear();
                selected_.insert(h);
                sel_anchor_ = h;
                has_sel_anchor_ = true;
                break;
        }
        if (changed_) changed_();
    }
    /// Is the flattened row i selected (by its handle)?
    [[nodiscard]] bool row_selected(std::size_t i) const {
        return i < flat_.size() && selected_.count(flat_[i].handle) != 0;
    }
    /// Handle-level selection access for the app.
    void set_selected(Handle h, bool on) {
        const bool changed = on ? selected_.insert(h).second
                                : (selected_.erase(h) != 0);
        if (changed && changed_) changed_();
    }
    [[nodiscard]] bool selected_contains(Handle h) const {
        return selected_.count(h) != 0;
    }
    [[nodiscard]] const std::unordered_set<Handle>& selected() const {
        return selected_;
    }
    void clear_selection() {
        if (selected_.empty()) return;
        selected_.clear();
        if (changed_) changed_();
    }
    /// The current flattened index of a handle, or npos when it is not
    /// visible (collapsed under a closed ancestor, or gone).
    static constexpr std::size_t npos = static_cast<std::size_t>(-1);
    [[nodiscard]] std::size_t index_of(Handle h) const {
        for (std::size_t i = 0; i < flat_.size(); ++i) {
            if (flat_[i].handle == h) return i;
        }
        return npos;
    }

    // ── Checked state (HANDLE-based, independent of selection) ──────────
    // Same identity rule as selection: keyed by handle so it survives
    // expand/collapse and can never migrate onto a different item.
    void set_checked(Handle h, bool on) {
        const bool changed = on ? checked_.insert(h).second
                                : (checked_.erase(h) != 0);
        if (changed && changed_) changed_();
    }
    [[nodiscard]] bool checked_contains(Handle h) const {
        return checked_.count(h) != 0;
    }
    [[nodiscard]] const std::unordered_set<Handle>& checked() const {
        return checked_;
    }

    /// Point a VirtualTreeProvider at this flattener: every tree question is
    /// answered from the flattened view + the expanded set. Call once.
    ///
    /// The item pointer is *never stored* — only handles are. At render the row
    /// resolves handle → item pointer through the locked data source, so a node
    /// deleted since the last flatten resolves to null and the row draws empty
    /// instead of dereferencing freed memory: the handle indirection is what
    /// makes the thing being drawn impossible to dangle.
    void wire(VirtualTreeProvider& provider) {
        rebuild();
        provider.on_item_count([this] { return flat_.size(); })
            .on_depth([this](std::size_t i) {
                return i < flat_.size() ? flat_[i].depth : 0;
            })
            .on_is_expandable([this](std::size_t i) {
                return i < flat_.size() && flat_[i].has_children;
            })
            .on_is_expanded([this](std::size_t i) {
                return i < flat_.size() &&
                       expanded_.count(flat_[i].handle) != 0;
            })
            .on_toggle([this](std::size_t i) {
                if (i >= flat_.size() || !flat_[i].has_children) return;
                const Handle h = flat_[i].handle;
                if (!expanded_.erase(h)) expanded_.insert(h);
                rebuild();
                if (changed_) changed_();
            })
            // Selection and checked state are wired to the flattener's
            // HANDLE-keyed models, so both survive expand/collapse (indices
            // renumber, handles don't). Re-set these AFTER wire() to
            // substitute a custom model. Checkbox RENDERING stays off until
            // the app turns it on: provider.checkboxes(true).
            .on_is_selected([this](std::size_t i) { return row_selected(i); })
            .on_activate(
                [this](std::size_t i, SelectMod m) { activate(i, m); })
            .on_is_checked([this](std::size_t i) {
                return i < flat_.size() &&
                       checked_.count(flat_[i].handle) != 0;
            })
            .on_set_checked([this](std::size_t i, bool on) {
                if (i < flat_.size()) set_checked(flat_[i].handle, on);
            });

        // Row content: prefer a rich render from the resolved item pointer;
        // otherwise fall back to the label text. Both resolve the handle fresh
        // (locked data) and guard null, so a stale node never dangles.
        if (render_ && resolve_) {
            provider.on_build_item([this](View& v, std::size_t i) {
                Item* item = item_at(i);  // handle → item, or null if gone
                if (item) render_(v, item);
            });
        } else {
            provider.on_item_text([this](std::size_t i) {
                Data* d = data_.lock();
                return (d && label_ && i < flat_.size())
                           ? label_(d, flat_[i].handle)
                           : std::string{};
            });
        }
    }

    /// The handle at flattened row i (for selection / drop mapping back to the
    /// app's own node identity). Returns Handle{} if out of range.
    [[nodiscard]] Handle handle_at(std::size_t i) const {
        return i < flat_.size() ? flat_[i].handle : Handle{};
    }
    [[nodiscard]] std::size_t size() const noexcept { return flat_.size(); }

    /// Recompute the flattened view. Call when the underlying tree *structure*
    /// changes (nodes added/removed) — expand/collapse already rebuilds itself.
    void rebuild() {
        flat_.clear();
        Data* d = data_.lock();
        if (!d || !roots_) return;
        std::vector<Handle> roots;
        roots_(d, roots);
        for (Handle r : roots) append(d, r, 0);
    }

private:
    struct Entry {
        Handle handle{};
        int    depth{0};
        bool   has_children{false};
    };

    void append(Data* d, Handle h, int depth) {
        const bool kids = has_children_ && has_children_(d, h);
        flat_.push_back({h, depth, kids});
        if (kids && expanded_.count(h) != 0 && children_) {
            std::vector<Handle> children;
            children_(d, h, children);
            for (Handle c : children) append(d, c, depth + 1);
        }
    }

    WeakRef<Data>            data_;
    RootsFn                  roots_;
    ChildrenFn               children_;
    LabelFn                  label_;
    HasChildrenFn            has_children_;
    ResolveFn                resolve_;
    RenderFn                 render_;
    ChangedFn                changed_;
    std::unordered_set<Handle> expanded_;
    std::unordered_set<Handle> selected_;
    std::unordered_set<Handle> checked_;
    Handle                   sel_anchor_{};
    bool                     has_sel_anchor_{false};
    std::vector<Entry>       flat_;
};

// --- Windowing math (templated so it accepts either provider — both expose the
// same size/count queries through the CRTP base) ----------------------------

template <typename P>
inline double virtual_offset(const P& p, std::size_t index, std::size_t count) {
    index = std::min(index, count);
    if (!p.has_variable_sizes()) {
        return static_cast<double>(index) * p.default_size();
    }
    double out = 0.0;
    for (std::size_t i = 0; i < index; ++i) out += p.item_size(i);
    return out;
}

template <typename P>
inline std::size_t virtual_item_at(const P& p, double pos, std::size_t count) {
    if (count == 0 || pos <= 0.0) return 0;
    if (!p.has_variable_sizes()) {
        const double size = p.default_size();
        if (size <= 0.0) return 0;
        const auto idx = static_cast<std::size_t>(pos / size);
        return std::min(idx, count > 0 ? count - 1 : 0);
    }
    double acc = 0.0;
    for (std::size_t i = 0; i < count; ++i) {
        acc += p.item_size(i);
        if (acc > pos) return i;
    }
    return count - 1;
}

template <typename P>
inline VirtualWindow compute_window(const P& p, std::int64_t scroll_px,
                                    double viewport, std::size_t overscan,
                                    std::size_t default_visible) {
    VirtualWindow w;
    const std::size_t count = p.item_count();
    w.total_px = virtual_offset(p, count, count);
    if (count == 0) return w;

    // Clamp the scroll offset into the honest extent.
    const double max_scroll = std::max(0.0, w.total_px - std::max(0.0, viewport));
    double pos = static_cast<double>(scroll_px);
    if (pos < 0.0) pos = 0.0;
    if (pos > max_scroll) pos = max_scroll;

    w.first = virtual_item_at(p, pos, count);

    // How many rows cover the viewport, plus one for the partial edge row.
    std::size_t visible;
    if (viewport > 0.0 && !p.has_variable_sizes()) {
        const double size = p.default_size();
        visible = static_cast<std::size_t>(viewport / std::max(1.0, size)) + 2;
    } else if (viewport > 0.0) {
        // Variable sizes: accumulate from `first` until the viewport is filled.
        double acc = 0.0;
        visible = 0;
        for (std::size_t i = w.first; i < count && acc < viewport; ++i) {
            acc += p.item_size(i);
            ++visible;
        }
        visible += 1;  // partial edge row
    } else {
        visible = default_visible;
    }

    w.begin = w.first > overscan ? w.first - overscan : 0;
    w.end   = std::min(count, w.first + visible + overscan);

    w.lead_px  = virtual_offset(p, w.begin, count);
    w.trail_px = std::max(0.0, w.total_px - virtual_offset(p, w.end, count));
    return w;
}

}  // namespace affineui
