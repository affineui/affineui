#pragma once

// ── DCC template: commands (a.k.a. actions) ─────────────────────────────────
//
// EXAMPLE / TEMPLATE CODE — not part of the AffineUI framework. This is a
// compact, best-practices spine of the kind shared by DCC tools (Maya, 3ds
// Max, Unity, Photoshop): a command-based editing core. Copy it into your app
// and extend it; AffineUI itself never depends on any of this.
//
// The model, kept deliberately small and *agnostic*:
//
//   An ACTION is a named thing that stores its arguments and knows how to
//   do / undo / redo itself.
//
// That is the whole contract. The base class does not care HOW a command
// captures the information it needs to undo — a subclass may snapshot state
// (memento), record an inverse edit, or append to a diff journal; all three
// derive from the same Command. Because every command has a stable name and a
// bag of named arguments, the same objects are the natural unit for scripting
// and replay later (Maya/Photoshop convert every interaction into a replayable
// command) — that hook is left as a clean seam, not built here.

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace app {

class Document;  // forward — commands edit a document (template's model).

/// A single argument value an action records. Kept to the handful of scalar
/// shapes a UI command actually carries; this is also what a future scripting
/// layer would serialise. (Replace with a richer variant as your app needs.)
using ArgValue = std::variant<std::monostate, bool, long long, double,
                              std::string>;

/// An ordered, named argument list — the action's "call signature". Ordered so
/// a replay/script layer can reproduce positional calls deterministically.
class Args {
public:
    Args& set(std::string key, ArgValue value);

    [[nodiscard]] bool has(std::string_view key) const noexcept;
    [[nodiscard]] const ArgValue* find(std::string_view key) const noexcept;

    [[nodiscard]] bool        get_bool(std::string_view key, bool fallback = false) const;
    [[nodiscard]] long long   get_int(std::string_view key, long long fallback = 0) const;
    [[nodiscard]] double      get_double(std::string_view key, double fallback = 0.0) const;
    [[nodiscard]] std::string get_string(std::string_view key,
                                         std::string_view fallback = {}) const;

    [[nodiscard]] const std::vector<std::pair<std::string, ArgValue>>&
    entries() const noexcept {
        return entries_;
    }

private:
    std::vector<std::pair<std::string, ArgValue>> entries_;
};

/// Base class for every editing action. Agnostic about the undo strategy: a
/// subclass implements redo()/undo() however it likes. `name()` is the stable
/// command id (also the scripting name); `label()` is the human, localised
/// text shown in menus and the undo history ("Undo <label>").
class Command {
public:
    Command() = default;
    explicit Command(std::string name, std::string label = {})
        : name_(std::move(name)), label_(std::move(label)) {}

    Command(const Command&)            = delete;
    Command& operator=(const Command&) = delete;
    virtual ~Command()                 = default;

    /// Apply the edit for the first time. Called once when the command runs.
    virtual void redo(Document& doc) = 0;
    /// Reverse the edit.
    virtual void undo(Document& doc) = 0;

    /// Whether this command changed anything worth pushing onto the undo
    /// stack. A command that turned out to be a no-op (e.g. set a value to what
    /// it already was) can return false to avoid polluting history. Default:
    /// always recorded.
    [[nodiscard]] virtual bool modifies() const { return true; }

    /// Coalescing: while the user drags a slider we generate many SetValue
    /// commands; merging consecutive same-target commands keeps the undo
    /// history to one entry. Return true if `next` was absorbed into this.
    [[nodiscard]] virtual bool merge_with(const Command& next) {
        (void)next;
        return false;
    }

    [[nodiscard]] std::string_view name() const noexcept { return name_; }
    [[nodiscard]] std::string_view label() const noexcept {
        return label_.empty() ? std::string_view{name_} : std::string_view{label_};
    }
    [[nodiscard]] const Args& args() const noexcept { return args_; }
    [[nodiscard]] Args&       args() noexcept { return args_; }

protected:
    std::string name_;
    std::string label_;
    Args        args_;
};

using CommandPtr = std::unique_ptr<Command>;

/// A command whose behaviour is supplied as lambdas — the easy path for simple
/// apps that don't want a subclass per action. Still a real Command (named,
/// argument-carrying, undoable), so it lives on the same stack and binds to the
/// same keys as heavier commands.
class LambdaCommand final : public Command {
public:
    using Apply = std::function<void(Document&)>;

    LambdaCommand(std::string name, std::string label, Apply redo, Apply undo)
        : Command(std::move(name), std::move(label)),
          redo_(std::move(redo)),
          undo_(std::move(undo)) {}

    void redo(Document& doc) override { if (redo_) redo_(doc); }
    void undo(Document& doc) override { if (undo_) undo_(doc); }

private:
    Apply redo_;
    Apply undo_;
};

}  // namespace app
