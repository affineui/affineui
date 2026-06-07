#include "app/document.h"

#include <algorithm>

namespace app {

const PropValue* Object::find(std::string_view prop) const {
    for (const auto& p : properties) {
        if (p.name == prop) return &p.value;
    }
    return nullptr;
}

PropValue* Object::find(std::string_view prop) {
    for (auto& p : properties) {
        if (p.name == prop) return &p.value;
    }
    return nullptr;
}

Object& Document::add(Object object) {
    objects_.push_back(std::move(object));
    return objects_.back();
}

Object* Document::find(std::string_view id) {
    for (auto& o : objects_) {
        if (o.id == id) return &o;
    }
    return nullptr;
}

const Object* Document::find(std::string_view id) const {
    for (const auto& o : objects_) {
        if (o.id == id) return &o;
    }
    return nullptr;
}

PropValue Document::property(std::string_view id, std::string_view prop,
                             PropValue fallback) const {
    if (const Object* o = find(id)) {
        if (const PropValue* v = o->find(prop)) return *v;
    }
    return fallback;
}

bool Document::set_property(std::string_view id, std::string_view prop,
                            PropValue value) {
    Object* o = find(id);
    if (!o) return false;
    if (PropValue* existing = o->find(prop)) {
        *existing = std::move(value);
    } else {
        o->properties.push_back({std::string(prop), std::move(value)});
    }
    return true;
}

void Document::rename(std::string_view id, std::string_view name) {
    if (Object* o = find(id)) o->name = std::string(name);
}

bool Document::remove(std::string_view id) {
    auto it = std::find_if(objects_.begin(), objects_.end(),
                           [&](const Object& o) { return o.id == id; });
    if (it == objects_.end()) return false;
    objects_.erase(it);
    return true;
}

void Document::touch() {
    dirty_ = true;
    if (changed_) changed_();
}

}  // namespace app
