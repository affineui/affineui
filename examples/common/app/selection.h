#pragma once

// ── DCC template: selection ─────────────────────────────────────────────────
//
// EXAMPLE / TEMPLATE CODE. A small ordered selection over object ids. Order is
// selection history; the last-added element is "active" (what inspectors and
// gizmos focus on). A change signal lets the UI refresh.

#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace app {

class Selection {
public:
    using ChangedFn = std::function<void()>;

    void select(std::string_view id);          // replace with one
    void add(std::string_view id);             // ctrl-click: add + make active
    void toggle(std::string_view id);          // ctrl-click existing/absent
    void clear();

    [[nodiscard]] bool contains(std::string_view id) const noexcept;
    [[nodiscard]] bool empty() const noexcept { return ids_.empty(); }
    [[nodiscard]] std::size_t size() const noexcept { return ids_.size(); }
    [[nodiscard]] std::string_view active() const noexcept;
    [[nodiscard]] const std::vector<std::string>& ids() const noexcept {
        return ids_;
    }

    void set_changed_handler(ChangedFn fn) { changed_ = std::move(fn); }

private:
    void notify() { if (changed_) changed_(); }

    std::vector<std::string> ids_;      // ordered by selection history
    std::string              active_;   // last-added element
    ChangedFn                changed_;
};

}  // namespace app
