#include "app/change_tracker.h"

#include "app/command_stack.h"

// SKELETON / STUBS. These bodies are intentionally inert: the change-tracking
// undo model is documented and its seam reserved, but not implemented. An app
// fills these in once it implements the affineui:: snapshot/diff/apply
// primitives for its own representation (see affineui/object.h). Until then the
// tracker records nothing, so callers that already mark objects compile and run
// with no undo side effects.

namespace app {

void ChangeTracker::mark_changed(const affineui::ObjectClass& cls, void* obj,
                                 std::string_view label) {
    // Intended: on first mark of `obj` in the open transaction, snapshot its
    // before-image (affineui::snapshot) and remember (cls, obj, before). Later
    // marks of the same obj are no-ops. Carry `label` for the eventual entry.
    (void)cls;
    (void)obj;
    (void)label;
    // STUB — no tracking yet.
}

void ChangeTracker::begin(std::string_view label) {
    // Intended: ++depth_; on the outermost begin, remember `label` for flush().
    (void)label;
    // STUB.
}

void ChangeTracker::end() {
    // Intended: if (--depth_ == 0) flush(label_).
    // STUB.
}

void ChangeTracker::end_frame() {
    // Intended: when no explicit transaction is open, flush(label_) to coalesce
    // everything marked this frame into one undo entry.
    // STUB.
}

void ChangeTracker::flush(std::string_view label) {
    // Intended: for each Tracked t, d = affineui::diff(*t.cls, t.before, t.obj);
    // collect non-empty diffs; if any, push ONE app::Command onto stack_ whose
    // undo applies affineui::apply(DiffSide::Old) to each and redo applies New.
    // Clear tracked_.
    (void)label;
    (void)stack_;
    tracked_.clear();
    // STUB — nothing pushed.
}

}  // namespace app
