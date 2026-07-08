#include "affineui/affineui.h"
#include "affineui/computed_style.h"

namespace affineui {

const char* version_string() noexcept {
    return AFFINEUI_VERSION_STRING;
}

// ComputedStyle::release lives here (not in computed_style.h) so the
// future delete of `ComputedStyleExtras` can see the complete type
// without dragging it into every public-header consumer.
void ComputedStyle::release() const noexcept {
    if (refcount_.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        delete this;
    }
}

int run(std::string_view html) {
    App app{};
    app.load_html(html);
    return app.run();
}

int run(std::function<void()> view_fn) {
    App app{};
    app.mount(std::move(view_fn));
    return app.run();
}

int run(App::Config cfg) {
    App app{std::move(cfg)};
    return app.run();
}

int run(App::Config cfg, std::function<void()> view_fn) {
    App app{std::move(cfg)};
    app.mount(std::move(view_fn));
    return app.run();
}

}  // namespace affineui
