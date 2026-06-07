#include "app/command_stack.h"

namespace app {

bool CommandStack::push(CommandPtr command) {
    if (!command) return false;

    // Apply the edit.
    command->redo(doc_);

    // A command that changed nothing runs but leaves no history entry.
    if (!command->modifies()) {
        notify();  // it may still have touched transient state worth redrawing
        return false;
    }

    // Discard any redo tail — we've branched history.
    if (head_ < commands_.size()) {
        commands_.erase(commands_.begin() +
                            static_cast<std::ptrdiff_t>(head_),
                        commands_.end());
    }

    // Coalesce into the current group's top command if it will absorb us.
    if (coalescing_ > 0 && coalesced_into_top_ && !commands_.empty()) {
        if (commands_.back()->merge_with(*command)) {
            notify();
            return true;
        }
    }

    commands_.push_back(std::move(command));
    head_ = commands_.size();
    if (coalescing_ > 0) coalesced_into_top_ = true;
    notify();
    return true;
}

std::string_view CommandStack::undo_label() const {
    if (!can_undo()) return {};
    return commands_[head_ - 1]->label();
}

std::string_view CommandStack::redo_label() const {
    if (!can_redo()) return {};
    return commands_[head_]->label();
}

bool CommandStack::undo() {
    if (!can_undo()) return false;
    --head_;
    commands_[head_]->undo(doc_);
    notify();
    return true;
}

bool CommandStack::redo() {
    if (!can_redo()) return false;
    commands_[head_]->redo(doc_);
    ++head_;
    notify();
    return true;
}

void CommandStack::clear() {
    commands_.clear();
    head_ = 0;
    coalescing_ = 0;
    coalesced_into_top_ = false;
    notify();
}

void CommandStack::begin_coalescing() {
    ++coalescing_;
}

void CommandStack::end_coalescing() {
    if (coalescing_ > 0) --coalescing_;
    if (coalescing_ == 0) coalesced_into_top_ = false;
}

void CommandStack::notify() {
    if (changed_) changed_();
}

}  // namespace app
