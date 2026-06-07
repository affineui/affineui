#pragma once

// ── DCC template: app context ───────────────────────────────────────────────
//
// EXAMPLE / TEMPLATE CODE. The small hub that wires the core pieces together
// and is handed to command factories and state predicates. It owns the
// document, selection, command stack, and registry; an app typically holds one
// Context and builds its UI/controllers around it. Run commands through run()
// so they go on the undo stack and the UI refreshes uniformly.
//
// This is the obvious place to extend with app-specific services (a viewport
// camera, a renderer handle, project paths) — keep those on a derived context
// or alongside this one.

#include <string>
#include <string_view>

#include "app/command.h"
#include "app/command_registry.h"
#include "app/command_stack.h"
#include "app/document.h"
#include "app/selection.h"

namespace app {

class Context {
public:
    Context() : stack_(document_) {}

    Context(const Context&)            = delete;
    Context& operator=(const Context&) = delete;

    [[nodiscard]] Document&        document() noexcept { return document_; }
    [[nodiscard]] const Document&  document() const noexcept { return document_; }
    [[nodiscard]] Selection&       selection() noexcept { return selection_; }
    [[nodiscard]] const Selection& selection() const noexcept { return selection_; }
    [[nodiscard]] CommandStack&    stack() noexcept { return stack_; }
    [[nodiscard]] const CommandStack& stack() const noexcept { return stack_; }
    [[nodiscard]] CommandRegistry& registry() noexcept { return registry_; }
    [[nodiscard]] const CommandRegistry& registry() const noexcept {
        return registry_;
    }

    /// Run an already-built command: push it on the stack (which applies it)
    /// and mark the document changed. Returns true if it was recorded.
    bool run(CommandPtr command);

    /// Build and run a registered command by id, with optional arguments.
    /// No-op (returns false) for an unknown or non-runnable id.
    bool run(std::string_view command_id, const Args& args = {});

private:
    Document        document_;
    Selection       selection_;
    CommandStack    stack_;
    CommandRegistry registry_;
};

}  // namespace app
