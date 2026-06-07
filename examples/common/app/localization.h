#pragma once

// ── DCC template: localization ──────────────────────────────────────────────
//
// EXAMPLE / TEMPLATE CODE. A deliberately small but real string-catalog: keys
// map to localised display strings, with a fallback locale and runtime locale
// switching. It is intentionally NOT a full i18n stack (no plurals, gender,
// ICU message syntax) — it is the shape a team would obviously extend, not a
// toy: catalogs are data, lookups are O(1), and missing keys degrade
// gracefully to the key itself so the UI is never blank.
//
//   auto& L = app::Localizer::instance();
//   L.add_locale("en", { {"menu.file", "File"}, {"cmd.undo", "Undo"} });
//   L.add_locale("fr", { {"menu.file", "Fichier"}, {"cmd.undo", "Annuler"} });
//   L.set_locale("fr");
//   panel.menu(app::tr("menu.file"));        // "Fichier"
//
// tr() is a free function over the process-wide Localizer for terse call sites.

#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace app {

class Localizer {
public:
    using Catalog = std::vector<std::pair<std::string, std::string>>;

    /// The process-wide localizer. (Single-threaded UI use, like the rest of
    /// the template.)
    static Localizer& instance();

    /// Register or extend a locale's catalog. Calling again merges/overrides.
    void add_locale(std::string locale, const Catalog& entries);

    /// Select the active locale. Unknown locales keep the current one.
    bool set_locale(std::string_view locale);
    [[nodiscard]] std::string_view locale() const noexcept { return active_; }

    /// The fallback locale consulted when a key is missing from the active one.
    void set_fallback_locale(std::string locale) {
        fallback_ = std::move(locale);
    }

    /// Look up `key` in the active locale, then the fallback locale; if still
    /// missing, return `key` itself so the UI shows *something* identifiable.
    [[nodiscard]] std::string_view text(std::string_view key) const;

    [[nodiscard]] const std::vector<std::string>& locales() const noexcept {
        return order_;
    }

private:
    std::unordered_map<std::string, std::unordered_map<std::string, std::string>>
        catalogs_;
    std::vector<std::string> order_;
    std::string active_{"en"};
    std::string fallback_{"en"};
};

/// Terse translation lookup over the process-wide localizer.
[[nodiscard]] inline std::string_view tr(std::string_view key) {
    return Localizer::instance().text(key);
}

}  // namespace app
