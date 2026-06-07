#include "app/standard_commands.h"

#include <string>
#include <vector>

namespace app {

namespace {

// Snapshot-based edit: removing objects records the removed objects (and their
// order) so undo can restore them. This is the "memento" flavour of command;
// other commands in an app may record inverse ops or diffs instead — the base
// class does not care.
class DeleteObjectsCommand final : public Command {
public:
    DeleteObjectsCommand(std::string label, std::vector<std::string> ids)
        : Command(std::string(cmd::delete_), std::move(label)),
          ids_(std::move(ids)) {
        for (const auto& id : ids_) args_.set("id", id);  // scripting record
    }

    void redo(Document& doc) override {
        removed_.clear();
        positions_.clear();
        const auto& objs = doc.objects();
        // Capture each target with its index so undo restores original order.
        for (std::size_t i = 0; i < objs.size(); ++i) {
            for (const auto& id : ids_) {
                if (objs[i].id == id) {
                    removed_.push_back(objs[i]);
                    positions_.push_back(i);
                    break;
                }
            }
        }
        for (const auto& id : ids_) doc.remove(id);
    }

    void undo(Document& doc) override {
        // Re-add in original index order (template Document appends; that's
        // fine for the outliner, which sorts by parent/depth, not raw order).
        for (auto& obj : removed_) doc.add(obj);
    }

    [[nodiscard]] bool modifies() const override { return !ids_.empty(); }

private:
    std::vector<std::string> ids_;
    std::vector<Object>      removed_;
    std::vector<std::size_t> positions_;
};

class DuplicateObjectsCommand final : public Command {
public:
    DuplicateObjectsCommand(std::string label, std::vector<std::string> ids)
        : Command(std::string(cmd::duplicate), std::move(label)),
          ids_(std::move(ids)) {}

    void redo(Document& doc) override {
        created_.clear();
        int n = 0;
        for (const auto& id : ids_) {
            const Object* src = doc.find(id);
            if (!src) continue;
            Object copy = *src;
            copy.id   = src->id + "_copy" + std::to_string(++n);
            copy.name = src->name + " Copy";
            doc.add(copy);
            created_.push_back(copy.id);
        }
    }

    void undo(Document& doc) override {
        for (const auto& id : created_) doc.remove(id);
    }

    [[nodiscard]] bool modifies() const override { return !ids_.empty(); }

    [[nodiscard]] const std::vector<std::string>& created() const noexcept {
        return created_;
    }

private:
    std::vector<std::string> ids_;
    std::vector<std::string> created_;
};

std::vector<std::string> selected_ids(const Context& ctx) {
    return ctx.selection().ids();
}

CommandState needs_selection(const Context& ctx) {
    CommandState s;
    s.enabled = !ctx.selection().empty();
    return s;
}

}  // namespace

void register_standard_commands(Context& ctx) {
    CommandRegistry& reg = ctx.registry();

    // Undo / Redo: registered for label + state + keybinding; dispatched
    // specially by dispatch_command (they drive the stack, not push onto it).
    reg.add({std::string(cmd::undo), "cmd.undo", "undo", nullptr,
             [](const Context& c) {
                 CommandState s;
                 s.enabled = c.stack().can_undo();
                 return s;
             }});
    reg.add({std::string(cmd::redo), "cmd.redo", "redo", nullptr,
             [](const Context& c) {
                 CommandState s;
                 s.enabled = c.stack().can_redo();
                 return s;
             }});

    reg.add({std::string(cmd::delete_), "cmd.delete", "trash",
             [](Context& c, const Args&) -> CommandPtr {
                 return std::make_unique<DeleteObjectsCommand>(
                     std::string(c.registry().label(cmd::delete_)),
                     selected_ids(c));
             },
             needs_selection});

    reg.add({std::string(cmd::duplicate), "cmd.duplicate", "copy",
             [](Context& c, const Args&) -> CommandPtr {
                 return std::make_unique<DuplicateObjectsCommand>(
                     std::string(c.registry().label(cmd::duplicate)),
                     selected_ids(c));
             },
             needs_selection});

    // Cut / Copy / Paste are stubbed against a per-app clipboard; here we model
    // cut as copy+delete and paste as duplicate of the clipboard ids. They are
    // wired enough to demonstrate the pattern; an app supplies real clipboard
    // storage. Copy alone is not undoable (it changes no document state).
    reg.add({std::string(cmd::copy), "cmd.copy", "copy", nullptr,
             needs_selection});
    reg.add({std::string(cmd::cut), "cmd.cut", "scissors",
             [](Context& c, const Args&) -> CommandPtr {
                 return std::make_unique<DeleteObjectsCommand>(
                     std::string(c.registry().label(cmd::cut)),
                     selected_ids(c));
             },
             needs_selection});
    reg.add({std::string(cmd::paste), "cmd.paste", "clipboard", nullptr,
             nullptr});

    reg.add({std::string(cmd::select_all), "cmd.selectAll", "", nullptr,
             nullptr});
}

void register_standard_keys(affineui::Keymap& keymap) {
    using K = affineui::Key;
    keymap.bind({K::Z, true, false, false, false}, std::string(cmd::undo));
    keymap.bind({K::Z, true, true, false, false}, std::string(cmd::redo));
    keymap.bind({K::Delete, false, false, false, false}, std::string(cmd::delete_));
    keymap.bind({K::D, true, false, false, false}, std::string(cmd::duplicate));
    keymap.bind({K::X, true, false, false, false}, std::string(cmd::cut));
    keymap.bind({K::C, true, false, false, false}, std::string(cmd::copy));
    keymap.bind({K::V, true, false, false, false}, std::string(cmd::paste));
    keymap.bind({K::A, true, false, false, false}, std::string(cmd::select_all));
}

Localizer::Catalog standard_command_labels_en() {
    return {
        {"cmd.undo", "Undo"},     {"cmd.redo", "Redo"},
        {"cmd.delete", "Delete"}, {"cmd.duplicate", "Duplicate"},
        {"cmd.cut", "Cut"},       {"cmd.copy", "Copy"},
        {"cmd.paste", "Paste"},   {"cmd.selectAll", "Select All"},
    };
}

}  // namespace app
