#include "app/selection.h"

#include <algorithm>

namespace app {

void Selection::select(std::string_view id) {
    ids_.assign(1, std::string(id));
    active_ = std::string(id);
    notify();
}

void Selection::add(std::string_view id) {
    if (!contains(id)) ids_.emplace_back(id);
    active_ = std::string(id);
    notify();
}

void Selection::toggle(std::string_view id) {
    auto it = std::find(ids_.begin(), ids_.end(), id);
    if (it == ids_.end()) {
        ids_.emplace_back(id);
        active_ = std::string(id);
    } else {
        ids_.erase(it);
        active_ = ids_.empty() ? std::string{} : ids_.back();
    }
    notify();
}

void Selection::clear() {
    ids_.clear();
    active_.clear();
    notify();
}

bool Selection::contains(std::string_view id) const noexcept {
    return std::find(ids_.begin(), ids_.end(), id) != ids_.end();
}

std::string_view Selection::active() const noexcept { return active_; }

}  // namespace app
