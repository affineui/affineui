#pragma once

// ── DCC template: undo/redo stack ───────────────────────────────────────────
//
// EXAMPLE / TEMPLATE CODE. Holds the linear history of executed commands and
// drives undo/redo. Running a new command after some undos discards the
// redo tail (the conventional, predictable behaviour).
//
// Coalescing: interactive edits (dragging a slider) emit a stream of commands
// that should collapse into one undo entry. push() asks the top-of-stack
// command to merge_with() the incoming one; if it absorbs it, no new entry is
// created. A coalescing "group" can also be opened explicitly around a gesture.

#include <functional>
#include <string_view>
#include <vector>

#include "app/command.h"

namespace app {

class Document;

class CommandStack {
public:
    /// Called after any change to the stack (push / undo / redo / clear) so the
    /// UI can refresh enabled-state of Undo/Redo and the history view.
    using ChangedFn = std::function<void()>;

    explicit CommandStack(Document& doc) : doc_(doc) {}

    /// Execute `command` (calls redo()) and record it for undo. If the command
    /// reports it changed nothing (modifies() == false) it runs but is not
    /// recorded. If coalescing is open and the top command absorbs this one,
    /// the history length is unchanged. Returns true if the stack changed.
    bool push(CommandPtr command);

    [[nodiscard]] bool can_undo() const noexcept { return head_ > 0; }
    [[nodiscard]] bool can_redo() const noexcept {
        return head_ < commands_.size();
    }

    /// Localised label of the command that undo()/redo() would act on, for
    /// "Undo Move" / "Redo Move" menu text. Empty when nothing to do.
    [[nodiscard]] std::string_view undo_label() const;
    [[nodiscard]] std::string_view redo_label() const;

    bool undo();
    bool redo();
    void clear();

    /// Open/close a coalescing group. While open, consecutive pushes that
    /// merge_with() the group's first command collapse into it. Typically
    /// wrapped around a drag gesture. Nesting is refcounted.
    void begin_coalescing();
    void end_coalescing();

    void set_changed_handler(ChangedFn fn) { changed_ = std::move(fn); }

    [[nodiscard]] std::size_t size() const noexcept { return commands_.size(); }
    [[nodiscard]] std::size_t head() const noexcept { return head_; }

private:
    void notify();

    Document&               doc_;
    std::vector<CommandPtr> commands_;
    std::size_t             head_{0};  // index where the next command goes;
                                       // [0,head_) are applied, [head_,end) redoable
    int                     coalescing_{0};
    bool                    coalesced_into_top_{false};
    ChangedFn               changed_;
};

}  // namespace app
