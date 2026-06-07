#pragma once

// ── Object reflection mediator ──────────────────────────────────────────────
//
// A tiny runtime reflection layer: a type participates as an AffineUI "object"
// by exposing a *class object* (its `ObjectClass`) that mediates access to its
// properties. Components (a property inspector, a data-bound field) talk to the
// class object, never to a concrete struct — so any user type can be inspected
// or bound without inheriting a base or boxing its state into a property bag.
//
// This is the deliberate seam for C++26 static reflection: today the property
// table is registered by hand with member pointers; when reflection ships the
// same `ObjectClass` can be synthesised from the type. The runtime shape that
// components depend on does not change.
//
// Crucially, the target type is NOT modified at all — no base class, no member,
// no field. A type opts in purely by an overloaded free function `get_class()`
// returning its class object, found by ADL:
//
//   struct Mesh { std::string name; double roughness; bool cast_shadows; };
//
//   const affineui::ObjectClass& get_class(const Mesh&) {
//       static const affineui::ObjectClass k =
//           affineui::ObjectClassBuilder<Mesh>("Mesh")
//               .property("name", &Mesh::name)
//               .property("roughness", &Mesh::roughness)
//               .property("castShadows", &Mesh::cast_shadows)
//               .build();
//       return k;
//   }
//
// So even a third-party / pre-existing type gets a class mediator with one free
// function in its own namespace — nothing about the type changes.
//
// Reflection and lifetime-tracking are orthogonal: this layer does NOT require
// a reflected type to be Trackable (see weak_ref.h). A plain value type works.
// But an object bound into *live* UI (a property inspector editing it) SHOULD
// also be Trackable, so the binding holds a weak handle and safely no-ops after
// the object is destroyed — that is the framework's hard-to-crash invariant.
// get_class() answers "what are its properties"; a WeakRef answers "is it still
// alive". A component editing an object wants both.

#include <any>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <type_traits>
#include <typeinfo>
#include <variant>
#include <vector>

#include "affineui/weak_ref.h"  // ObjectBase is Trackable

namespace affineui {

/// The value a reflected property can carry. Kept deliberately small and
/// aligned with what editor fields produce (bool ↔ checkbox/switch, double ↔
/// slider/knob/number, string ↔ text/colour/enum). Extend per need.
using PropertyValue = std::variant<bool, double, std::string>;

namespace detail {
// Box a concrete member value into a PropertyValue. Integral/floating types
// fold to double; string-like to std::string; bool stays bool.
template <class M>
PropertyValue to_property_value(const M& m) {
    if constexpr (std::is_same_v<M, bool>) {
        return PropertyValue{m};
    } else if constexpr (std::is_integral_v<M> || std::is_floating_point_v<M>) {
        return PropertyValue{static_cast<double>(m)};
    } else {
        return PropertyValue{std::string(m)};
    }
}

// Write a PropertyValue back into a concrete member (best-effort, never throws
// on a type mismatch — a wrong-typed value is simply ignored).
template <class M>
void from_property_value(M& m, const PropertyValue& v) {
    if constexpr (std::is_same_v<M, bool>) {
        if (const auto* b = std::get_if<bool>(&v)) m = *b;
    } else if constexpr (std::is_integral_v<M> || std::is_floating_point_v<M>) {
        if (const auto* d = std::get_if<double>(&v)) m = static_cast<M>(*d);
    } else {  // string-like
        if (const auto* s = std::get_if<std::string>(&v)) m = *s;
    }
}
}  // namespace detail

/// Property metadata works like C# attributes: typed annotations attached to a
/// property and queried by type. These standard ones drive a tools inspector;
/// apps can attach their own attribute types too.
namespace attr {
struct Label    { std::string text; };          // display label override
struct Tooltip  { std::string text; };
struct Range    { double min{0.0}, max{1.0}, step{0.01}; };  // numeric bounds
struct Slider   {};                              // render numeric as a slider
struct Color    {};                              // render string as a colour field
struct ReadOnly {};                              // not editable
}  // namespace attr

/// One property's runtime descriptor: a name, a set of typed attributes (C#
/// style), and type-erased accessors over an object of the owning class. A
/// default-constructed (invalid) descriptor is what an out-of-range lookup
/// returns — every accessor here stays safe on it.
struct PropertyInfo {
    std::string                                      name;
    std::vector<std::any>                            attributes;
    std::function<PropertyValue(const void*)>        get;
    std::function<void(void*, const PropertyValue&)> set;

    [[nodiscard]] bool valid() const noexcept { return !name.empty(); }

    /// Query an attribute by type (like C#'s GetCustomAttribute<T>()); null if
    /// absent.
    template <class A>
    [[nodiscard]] const A* attribute() const noexcept {
        for (const auto& a : attributes)
            if (const A* p = std::any_cast<A>(&a)) return p;
        return nullptr;
    }
    template <class A>
    [[nodiscard]] bool has() const noexcept {
        return attribute<A>() != nullptr;
    }
    [[nodiscard]] std::string_view display_label() const noexcept {
        if (const auto* l = attribute<attr::Label>()) return l->text;
        return name;
    }
};

/// The class object — the reflection mediator for one concrete type. Property
/// access is intentionally just `property_count()` + `property_at(index)`,
/// returning a value copy; both are always safe (an out-of-range index yields
/// an invalid descriptor). No internal container or pointer is ever exposed.
class ObjectClass {
public:
    ObjectClass() = default;
    ObjectClass(std::string name, const std::type_info& type,
                std::vector<PropertyInfo> props)
        : name_(std::move(name)), type_(&type), props_(std::move(props)) {}

    [[nodiscard]] std::string_view name() const noexcept { return name_; }
    /// The reflected type's identity. Objects in this system are required to be
    /// RTTI-identifiable (they need a typeid); they do NOT need names or ids.
    [[nodiscard]] const std::type_info& type_id() const noexcept {
        return type_ ? *type_ : typeid(void);
    }

    [[nodiscard]] std::size_t property_count() const noexcept {
        return props_.size();
    }
    /// The property at `index`, by value. Safe: an out-of-range index returns an
    /// invalid (default) descriptor rather than reading out of bounds.
    [[nodiscard]] PropertyInfo property_at(std::size_t index) const {
        return index < props_.size() ? props_[index] : PropertyInfo{};
    }

    /// Read a property value by name. Empty value (false) if the name is absent.
    [[nodiscard]] PropertyValue get(const void* obj, std::string_view prop) const {
        for (const auto& p : props_)
            if (p.name == prop) return p.get ? p.get(obj) : PropertyValue{false};
        return PropertyValue{false};
    }
    /// Write a property value by name. Returns false if the name is absent.
    bool set(void* obj, std::string_view prop, const PropertyValue& value) const {
        for (const auto& p : props_)
            if (p.name == prop) {
                if (!p.set) return false;
                p.set(obj, value);
                return true;
            }
        return false;
    }

private:
    std::string               name_;
    const std::type_info*     type_{&typeid(void)};
    std::vector<PropertyInfo> props_;
};

/// Fluent builder for an ObjectClass. Register properties with member pointers;
/// the get/set accessors are generated and type-erased.
template <class T>
class ObjectClassBuilder {
public:
    explicit ObjectClassBuilder(std::string name) : name_(std::move(name)) {}

    /// Register a property backed by a member, with any number of trailing
    /// attributes (`attr::Range{0,1}`, `attr::Slider{}`, `attr::Color{}`, a
    /// custom one, …).
    template <class M, class... Attrs>
    ObjectClassBuilder& property(std::string prop, M T::*member, Attrs&&... attrs) {
        PropertyInfo info;
        info.name = std::move(prop);
        info.attributes = make_attributes(std::forward<Attrs>(attrs)...);
        info.get = [member](const void* o) {
            return detail::to_property_value(static_cast<const T*>(o)->*member);
        };
        info.set = [member](void* o, const PropertyValue& v) {
            detail::from_property_value(static_cast<T*>(o)->*member, v);
        };
        props_.push_back(std::move(info));
        return *this;
    }

    /// Register a property backed by free getter/setter callables (computed or
    /// non-member storage), with trailing attributes. Pass an empty setter for
    /// a read-only/computed property.
    template <class... Attrs>
    ObjectClassBuilder& computed(std::string prop,
                                 std::function<PropertyValue(const T&)> getter,
                                 std::function<void(T&, const PropertyValue&)> setter,
                                 Attrs&&... attrs) {
        PropertyInfo info;
        info.name = std::move(prop);
        info.attributes = make_attributes(std::forward<Attrs>(attrs)...);
        info.get = [g = std::move(getter)](const void* o) {
            return g(*static_cast<const T*>(o));
        };
        if (setter) {
            info.set = [s = std::move(setter)](void* o, const PropertyValue& v) {
                s(*static_cast<T*>(o), v);
            };
        }
        props_.push_back(std::move(info));
        return *this;
    }

    [[nodiscard]] ObjectClass build() {
        return ObjectClass{std::move(name_), typeid(T), std::move(props_)};
    }

private:
    template <class... Attrs>
    static std::vector<std::any> make_attributes(Attrs&&... attrs) {
        std::vector<std::any> out;
        out.reserve(sizeof...(Attrs));
        (out.emplace_back(std::forward<Attrs>(attrs)), ...);
        return out;
    }

    std::string               name_;
    std::vector<PropertyInfo> props_;
};

/// A type is Reflectable if a `get_class()` overload yields its class object
/// (found by ADL — no member/base required on the type). The class object is
/// typically a function-local static.
template <class T>
concept Reflectable = requires(const T& o) {
    { get_class(o) } -> std::convertible_to<const ObjectClass&>;
};

/// Convenience: typed get/set on a reflectable instance.
template <Reflectable T>
[[nodiscard]] PropertyValue get_property(const T& obj, std::string_view prop) {
    return get_class(obj).get(&obj, prop);
}
template <Reflectable T>
bool set_property(T& obj, std::string_view prop, const PropertyValue& value) {
    return get_class(obj).set(&obj, prop, value);
}

/// An optional convenience base for app objects: it is Trackable (so live UI
/// bindings to it are crash-safe) and carries a virtual class-getter, so a
/// derived type automatically conforms to the reflection protocol — no separate
/// get_class() overload needed. (Deriving this is never required: a plain type
/// opts in with a free get_class() instead.)
class ObjectBase : public Trackable {
public:
    /// The derived type's class object. Typically returns a function-local
    /// static built once with ObjectClassBuilder<Derived>.
    [[nodiscard]] virtual const ObjectClass& object_class() const = 0;
};

/// Bridges ObjectBase into the Reflectable protocol: found by ADL for any type
/// derived from ObjectBase (the base is an associated class), it forwards to the
/// virtual class-getter — so the dynamic type's ObjectClass is used.
[[nodiscard]] inline const ObjectClass& get_class(const ObjectBase& obj) {
    return obj.object_class();
}

}  // namespace affineui
