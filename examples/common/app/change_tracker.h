#pragma once

// ── DCC template: change tracker (SKELETON / STUBS) ─────────────────────────
//
// EXAMPLE / TEMPLATE CODE — the intended undo/redo architecture, sketched.
// NONE of this is implemented; it documents the contract and reserves the seam
// so an app can fill in the bodies. The framework already provides the base
// types (affineui::ObjectSnapshot / ObjectDiff / DiffSide) and the
// snapshot/diff/apply signatures over any Reflectable type; this shell layer
// turns those into a transaction-scoped undo entry.
//
// The model (change-tracking by DIFF, not per-field commands):
//
//   1. An edit that is about to touch an object calls mark_changed(cls, obj).
//      On the FIRST mark of that object within the open transaction, the
//      tracker snapshots a "before" image (affineui::snapshot). Subsequent
//      marks of the same object in the same transaction are cheap no-ops.
//   2. When the transaction closes, every marked object is diffed against its
//      current state (affineui::diff). The union of non-empty diffs becomes
//      ONE undo entry pushed on the CommandStack: undo applies the Old side of
//      each diff, redo the New side (affineui::apply).
//
// So editing code just marks + mutates — it never writes undo/redo by hand.
// Structural ops that a property diff can't express (add / delete / reparent)
// still use explicit app::Command as before; the two mechanisms coexist.
//
// Transaction boundary — frame-default with explicit override:
//   * By default the tracker auto-flushes at end of frame: everything marked
//     during the frame collapses into one entry (call end_frame() from the
//     app's frame callback).
//   * An explicit begin()/end() scopes a multi-frame gesture (a drag) into a
//     single entry, overriding the per-frame flush until end() is called.
//
// The snapshot/diff payloads are OPAQUE (see affineui/object.h): the tracker
// holds and forwards them without interpreting them. An app implementing the
// affineui:: primitives picks the representation (text, binary, delta) freely.

#include <string>
#include <string_view>
#include <vector>

#include "affineui/object.h"

namespace app {

class CommandStack;

/// Transaction-scoped change tracker. SKELETON: the interface is real, the
/// bodies are stubs (see change_tracker.cpp) — an app implements them once its
/// snapshot/diff representation exists. Constructed over the stack it pushes
/// coalesced diff-undo entries onto.
class ChangeTracker {
public:
    explicit ChangeTracker(CommandStack& stack) : stack_(stack) {}

    ChangeTracker(const ChangeTracker&)            = delete;
    ChangeTracker& operator=(const ChangeTracker&) = delete;

    /// Mark a reflected object as about-to-change. First mark in the open
    /// transaction snapshots its "before" image; later marks are no-ops.
    /// Convenience overload for any Reflectable T (uses get_class(obj)).
    template <affineui::Reflectable T>
    void mark_changed(T& obj) {
        mark_changed(affineui::get_class(obj), &obj);
    }
    /// Type-erased mark: `cls` reflects `obj`; used when the caller already
    /// holds the ObjectClass (e.g. an e3d node whose class is fetched once).
    void mark_changed(const affineui::ObjectClass& cls, void* obj,
                      std::string_view label = {});

    /// Open an explicit transaction. Marks until end() collapse into one undo
    /// entry, overriding the per-frame flush. Refcounted for nesting.
    void begin(std::string_view label = {});
    /// Close the current explicit transaction, flushing its diffs as one entry.
    void end();

    /// Per-frame auto-flush: diff every object marked this frame that isn't
    /// inside an open explicit transaction, and push one coalesced entry.
    /// Call once from the app's frame callback.
    void end_frame();

    /// True while an explicit transaction is open.
    [[nodiscard]] bool in_transaction() const noexcept { return depth_ > 0; }

private:
    // One tracked object's before-image within the current transaction.
    struct Tracked {
        const affineui::ObjectClass* cls{nullptr};
        void*                        obj{nullptr};
        affineui::ObjectSnapshot     before;
    };

    void flush(std::string_view label);  // diff + push one entry (STUB body)

    CommandStack&        stack_;
    std::vector<Tracked> tracked_;   // marked-this-transaction objects
    std::string          label_;     // explicit-transaction label
    int                  depth_{0};  // explicit begin/end nesting
};

}  // namespace app
