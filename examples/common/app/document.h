#pragma once

// ── DCC template: document model ────────────────────────────────────────────
//
// EXAMPLE / TEMPLATE CODE. A small scene document: a list of objects, each an
// id + type + a handful of named, typed properties. Commands edit the document;
// the document raises a change signal so the UI rebuilds. Plain value semantics
// throughout — the document is the single owner of scene state.
//
// Introspection is intentionally minimal and hand-rolled: objects expose their
// properties as {name, value} pairs, which is enough to drive a generic
// inspector and (later) save/load. This is the seam to replace with C++26
// static reflection when it ships — we deliberately avoid heavyweight C++17
// reflection metaprogramming for something the language is about to provide.
//
// NOTE (design in progress): this concrete "scene of property-bag objects" is
// being re-thought as opt-in LAYERS — see affineui/object.h (reflection) and
// the docs — so the agnostic template spine (commands/undo/selection) does not
// force an inspectable-object paradigm on documents that are not (a spreadsheet,
// a timeline, …). Until that layering lands, this remains the model.

#include <functional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace app {

/// The value types a scene property can hold. (Extend per app.)
using PropValue = std::variant<bool, double, std::string>;

struct Property {
    std::string name;
    PropValue   value;
};

/// One scene object: a stable id, a type tag (mesh/light/…), a display name,
/// and its properties. A tree is expressed with `parent` + `depth` for the
/// outliner without a separate node graph.
struct Object {
    std::string id;
    std::string type;
    std::string name;
    std::string parent;       // empty for roots
    int         depth{0};
    std::vector<Property> properties;

    [[nodiscard]] const PropValue* find(std::string_view prop) const;
    [[nodiscard]] PropValue*       find(std::string_view prop);
};

class Document {
public:
    using ChangedFn = std::function<void()>;

    Document() = default;

    Object&       add(Object object);
    [[nodiscard]] Object*       find(std::string_view id);
    [[nodiscard]] const Object* find(std::string_view id) const;
    [[nodiscard]] const std::vector<Object>& objects() const noexcept {
        return objects_;
    }

    /// Property get/set — the path commands use to edit. set_property is a
    /// quiet write (no signal); call touch() once after a batch, or use the
    /// notifying overload for single edits.
    [[nodiscard]] PropValue property(std::string_view id, std::string_view prop,
                                     PropValue fallback) const;
    bool set_property(std::string_view id, std::string_view prop,
                      PropValue value);

    void rename(std::string_view id, std::string_view name);
    bool remove(std::string_view id);

    [[nodiscard]] std::string_view title() const noexcept { return title_; }
    void set_title(std::string title) { title_ = std::move(title); }
    [[nodiscard]] bool dirty() const noexcept { return dirty_; }
    void set_dirty(bool dirty) { dirty_ = dirty; }

    /// Mark the document changed and fire the signal (called by commands).
    void touch();
    void set_changed_handler(ChangedFn fn) { changed_ = std::move(fn); }

private:
    std::vector<Object> objects_;
    std::string         title_{"Untitled"};
    bool                dirty_{false};
    ChangedFn           changed_;
};

}  // namespace app
