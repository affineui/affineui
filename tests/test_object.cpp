#include <doctest/doctest.h>

#include "affineui/object.h"

#include <ostream>  // doctest needs a streamable type for string CHECKs
#include <string>

namespace {

// A plain pre-existing type. It is NOT modified for reflection: no base class,
// no members added, no macros — exactly as a third-party type would arrive.
struct Mesh {
    std::string name;
    double      roughness{0.5};
    bool        cast_shadows{true};
};

// The type opts in with a single overloaded free function, found by ADL.
const affineui::ObjectClass& get_class(const Mesh&) {
    using namespace affineui::attr;
    static const affineui::ObjectClass k =
        affineui::ObjectClassBuilder<Mesh>("Mesh")
            .property("name", &Mesh::name, Label{"Object Name"})
            .property("roughness", &Mesh::roughness, Slider{}, Range{0.0, 1.0})
            .property("castShadows", &Mesh::cast_shadows)
            .build();
    return k;
}

// A computed property backed by getter/setter callables (no member storage).
struct Light {
    double intensity{1.0};
};
const affineui::ObjectClass& get_class(const Light&) {
    static const affineui::ObjectClass k =
        affineui::ObjectClassBuilder<Light>("Light")
            .computed(
                "intensity",
                [](const Light& l) { return affineui::PropertyValue{l.intensity}; },
                [](Light& l, const affineui::PropertyValue& v) {
                    if (const auto* d = std::get_if<double>(&v)) l.intensity = *d;
                },
                affineui::attr::Range{0.0, 2.0})
            .computed(
                "bright",  // computed, read-only (empty setter)
                [](const Light& l) {
                    return affineui::PropertyValue{l.intensity > 0.5};
                },
                {})
            .build();
    return k;
}

// An object using the convenience base: Trackable + auto-conforming, no
// separate get_class() overload needed.
struct Widget : affineui::ObjectBase {
    int count{0};
    [[nodiscard]] const affineui::ObjectClass& object_class() const override {
        static const affineui::ObjectClass k =
            affineui::ObjectClassBuilder<Widget>("Widget")
                .property("count", &Widget::count)
                .build();
        return k;
    }
};

}  // namespace

TEST_CASE("ObjectClass mediates property access without modifying the type") {
    static_assert(affineui::Reflectable<Mesh>,
                  "Mesh opts in via a free get_class() overload");

    Mesh m{"Hero", 0.62, true};
    const affineui::ObjectClass& cls = get_class(m);
    CHECK(cls.name() == "Mesh");
    // Property access is count + indexed (the only access methods).
    CHECK(cls.property_count() == 3);

    // Read through the mediator.
    CHECK(std::get<std::string>(affineui::get_property(m, "name")) == "Hero");
    CHECK(std::get<double>(affineui::get_property(m, "roughness")) ==
          doctest::Approx(0.62));
    CHECK(std::get<bool>(affineui::get_property(m, "castShadows")) == true);

    // Write through the mediator → the real member changes.
    affineui::set_property(m, "roughness", affineui::PropertyValue{0.9});
    CHECK(m.roughness == doctest::Approx(0.9));
    affineui::set_property(m, "name",
                           affineui::PropertyValue{std::string("Villain")});
    CHECK(m.name == "Villain");

    // Enumeration by index, in registration order.
    CHECK(cls.property_at(0).name == "name");
    CHECK(cls.property_at(2).name == "castShadows");

    // An out-of-range index is safe: an invalid descriptor, no UB.
    CHECK_FALSE(cls.property_at(99).valid());

    // Absent property: get yields the empty value, set reports failure.
    CHECK(std::get<bool>(cls.get(&m, "missing")) == false);
    CHECK_FALSE(cls.set(&m, "missing", affineui::PropertyValue{1.0}));
}

TEST_CASE("properties carry C#-style typed attributes queried by type") {
    Mesh m;
    const affineui::ObjectClass& cls = get_class(m);

    // name has a Label override; roughness has Slider + Range; castShadows none.
    const affineui::PropertyInfo name = cls.property_at(0);
    CHECK(name.display_label() == "Object Name");
    CHECK_FALSE(name.has<affineui::attr::Slider>());

    const affineui::PropertyInfo rough = cls.property_at(1);
    CHECK(rough.has<affineui::attr::Slider>());
    const auto* range = rough.attribute<affineui::attr::Range>();
    REQUIRE(range != nullptr);
    CHECK(range->min == doctest::Approx(0.0));
    CHECK(range->max == doctest::Approx(1.0));

    const affineui::PropertyInfo shadows = cls.property_at(2);
    CHECK(shadows.display_label() == "castShadows");  // falls back to the name
    CHECK(shadows.attribute<affineui::attr::Range>() == nullptr);
}

TEST_CASE("ObjectClass supports computed / getter-setter properties") {
    Light l{0.62};
    CHECK(std::get<double>(affineui::get_property(l, "intensity")) ==
          doctest::Approx(0.62));
    CHECK(std::get<bool>(affineui::get_property(l, "bright")) == true);

    affineui::set_property(l, "intensity", affineui::PropertyValue{0.1});
    CHECK(l.intensity == doctest::Approx(0.1));
    CHECK(std::get<bool>(affineui::get_property(l, "bright")) == false);

    // The read-only computed property has no setter — set is a no-op failure.
    CHECK_FALSE(get_class(l).set(&l, "bright", affineui::PropertyValue{true}));
}

TEST_CASE("a wrong-typed value write is ignored, not a crash") {
    Mesh m{"X", 0.5, false};
    // roughness is double; writing a string must not throw or corrupt it.
    affineui::set_property(m, "roughness",
                           affineui::PropertyValue{std::string("nope")});
    CHECK(m.roughness == doctest::Approx(0.5));
}

TEST_CASE("ObjectClass records the reflected type's typeid (no name/id needed)") {
    Mesh m;
    CHECK(get_class(m).type_id() == typeid(Mesh));
}

TEST_CASE("ObjectBase auto-conforms to the protocol and is trackable") {
    static_assert(affineui::Reflectable<Widget>,
                  "deriving ObjectBase opts into reflection automatically");
    static_assert(affineui::WeaklyTrackable<Widget>,
                  "ObjectBase is Trackable, so bindings are crash-safe");

    Widget w;
    w.count = 7;
    // The get_class(ObjectBase&) bridge resolves to the dynamic class.
    CHECK(affineui::get_class(w).name() == "Widget");
    CHECK(affineui::get_class(w).type_id() == typeid(Widget));
    CHECK(std::get<double>(affineui::get_property(w, "count")) ==
          doctest::Approx(7));

    // A weak handle to it works and survives use (the stability win).
    auto ref = affineui::to_weak_ref(&w);
    CHECK(ref.alive());
}
