#pragma once

// ── DCC template: preferences + workspace state ─────────────────────────────
//
// EXAMPLE / TEMPLATE CODE. Two deliberately-separate persisted stores, because
// they have different lifecycles and the user thinks of them differently:
//
//   Preferences — durable user config: theme, accent, keybindings, paths,
//                 "ask before quit". Changed intentionally; lives in a config
//                 location; survives forever.
//
//   Workspace   — ephemeral UI/layout state: arrangement of dockable windows,
//                 which panels are open, panel sizes, last-active tab/tool.
//                 Saved on exit, restored on launch; disposable.
//
// Both are a thin typed facade over the tiny TOML store (toml.h): dotted-key
// get/set with typed accessors, plus load()/save() to a file path. Keeping them
// as separate types (and separate files) makes the distinction obvious and lets
// an app blow away workspace state without touching preferences.

#include <string>
#include <string_view>

#include "app/toml.h"

namespace app {

/// Common typed get/set + file load/save over a TOML root table. Not used
/// directly; Preferences and Workspace derive from it so they are distinct
/// types with distinct files but share the (trivial) plumbing.
class SettingsStore {
public:
    [[nodiscard]] bool        get_bool(std::string_view key, bool fb = false) const;
    [[nodiscard]] long long   get_int(std::string_view key, long long fb = 0) const;
    [[nodiscard]] double      get_double(std::string_view key, double fb = 0.0) const;
    [[nodiscard]] std::string get_string(std::string_view key,
                                         std::string_view fb = {}) const;
    [[nodiscard]] bool        has(std::string_view key) const;

    void set_bool(std::string_view key, bool value);
    void set_int(std::string_view key, long long value);
    void set_double(std::string_view key, double value);
    void set_string(std::string_view key, std::string_view value);

    /// Load from `path` (replacing current contents). Missing file is fine —
    /// returns false but leaves the store empty/usable. Never throws.
    bool load(const std::string& path);
    /// Write to `path`. Returns false on I/O failure. Never throws.
    bool save(const std::string& path) const;

    [[nodiscard]] const toml::Table& table() const noexcept { return root_; }
    [[nodiscard]] toml::Table&       table() noexcept { return root_; }

protected:
    toml::Table root_;
};

/// Durable user configuration.
class Preferences : public SettingsStore {};

/// Ephemeral UI / window-arrangement state. Same plumbing, different file and
/// lifecycle — kept a separate type on purpose.
class Workspace : public SettingsStore {
public:
    // Convenience for the most common workspace facts. (Apps add their own.)
    [[nodiscard]] bool panel_open(std::string_view panel_id, bool fb = true) const;
    void set_panel_open(std::string_view panel_id, bool open);
    [[nodiscard]] int  panel_size(std::string_view panel_id, int fb) const;
    void set_panel_size(std::string_view panel_id, int size);
};

}  // namespace app
