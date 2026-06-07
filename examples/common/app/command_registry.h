#pragma once

// ── DCC template: command registry + command state ──────────────────────────
//
// EXAMPLE / TEMPLATE CODE. The catalog of commands an app exposes by id. Menus,
// toolbars, and keybindings all refer to a command by its stable id; the
// registry knows how to build/run it and how to query its live state so the UI
// can grey out unavailable commands and show toggles checked.
//
// A command's runtime state (enabled / checked / visible) is computed on demand
// from the app's current state via small predicates — the "command query"
// pattern from Maya/Max, kept to three booleans. Predicates default to
// "enabled, unchecked, visible" so simple commands need no state code.

#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "app/command.h"

namespace app {

class Context;  // the app context handed to factories/predicates (below).

struct CommandState {
    bool enabled{true};
    bool checked{false};
    bool visible{true};
};

/// Metadata + behaviour for one registered command.
struct CommandInfo {
    std::string id;
    std::string label_key;   // localization key; resolved through tr()
    std::string icon;        // optional icon name for menus/toolbars

    /// Build a fresh command instance to run (with the given arguments). May be
    /// null for commands that are dispatched some other way.
    std::function<CommandPtr(Context&, const Args&)> make;

    /// Compute live state. Default leaves it enabled/unchecked/visible.
    std::function<CommandState(const Context&)> state;
};

class CommandRegistry {
public:
    void add(CommandInfo info);

    [[nodiscard]] const CommandInfo* find(std::string_view id) const;
    [[nodiscard]] const std::vector<CommandInfo>& all() const noexcept {
        return infos_;
    }

    /// Resolve a command's localized label and live state for the UI.
    [[nodiscard]] std::string_view label(std::string_view id) const;
    [[nodiscard]] CommandState state(std::string_view id,
                                     const Context& ctx) const;

private:
    std::vector<CommandInfo>                       infos_;
    std::unordered_map<std::string, std::size_t>   index_;  // id -> infos_ pos
};

}  // namespace app
