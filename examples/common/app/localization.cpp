#include "app/localization.h"

#include <algorithm>

namespace app {

Localizer& Localizer::instance() {
    static Localizer localizer;
    return localizer;
}

void Localizer::add_locale(std::string locale, const Catalog& entries) {
    if (std::find(order_.begin(), order_.end(), locale) == order_.end()) {
        order_.push_back(locale);
    }
    auto& catalog = catalogs_[locale];
    for (const auto& [key, value] : entries) {
        catalog[key] = value;
    }
}

bool Localizer::set_locale(std::string_view locale) {
    auto it = catalogs_.find(std::string(locale));
    if (it == catalogs_.end()) return false;
    active_ = std::string(locale);
    return true;
}

std::string_view Localizer::text(std::string_view key) const {
    const std::string key_str(key);

    auto active_it = catalogs_.find(active_);
    if (active_it != catalogs_.end()) {
        auto entry = active_it->second.find(key_str);
        if (entry != active_it->second.end()) return entry->second;
    }
    if (fallback_ != active_) {
        auto fallback_it = catalogs_.find(fallback_);
        if (fallback_it != catalogs_.end()) {
            auto entry = fallback_it->second.find(key_str);
            if (entry != fallback_it->second.end()) return entry->second;
        }
    }
    // Missing everywhere: echo the key so the UI is identifiable, not blank.
    return key;
}

}  // namespace app
