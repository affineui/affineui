#include <doctest/doctest.h>

#include "app/context.h"
#include "app/gesture.h"
#include "affineui/keymap.h"
#include "app/localization.h"
#include "app/settings.h"
#include "app/standard_commands.h"
#include "app/toml.h"

#include <cstdio>
#include <filesystem>
#include <memory>
#include <ostream>
#include <string>

using namespace app;

namespace {

Object make_obj(std::string id, std::string name) {
    Object o;
    o.id = std::move(id);
    o.type = "mesh";
    o.name = std::move(name);
    o.properties.push_back({"roughness", PropValue{0.5}});
    return o;
}

// A minimal concrete command: set a numeric property, capturing the old value
// for undo (the memento flavour) and coalescing consecutive edits of the same
// target — what a slider drag needs.
class SetRoughness final : public Command {
public:
    SetRoughness(std::string id, double value)
        : Command("obj.setRoughness", "Set Roughness"),
          id_(std::move(id)),
          new_(value) {
        args_.set("id", id_).set("value", value);
    }
    void redo(Document& doc) override {
        old_ = std::get<double>(doc.property(id_, "roughness", PropValue{0.0}));
        doc.set_property(id_, "roughness", PropValue{new_});
    }
    void undo(Document& doc) override {
        doc.set_property(id_, "roughness", PropValue{old_});
    }
    bool merge_with(const Command& next) override {
        if (next.name() != name()) return false;
        const auto* other = dynamic_cast<const SetRoughness*>(&next);
        if (!other || other->id_ != id_) return false;
        new_ = other->new_;  // absorb the newer value; keep our captured old_
        return true;
    }
private:
    std::string id_;
    double new_{0.0};
    double old_{0.0};
};

double roughness(const Document& doc, std::string_view id) {
    return std::get<double>(doc.property(id, "roughness", PropValue{-1.0}));
}

}  // namespace

TEST_CASE("command runs and undo/redo round-trips through the stack") {
    Context ctx;
    ctx.document().add(make_obj("cube", "Cube"));

    ctx.run(std::make_unique<SetRoughness>("cube", 0.8));
    CHECK(roughness(ctx.document(), "cube") == doctest::Approx(0.8));
    CHECK(ctx.stack().can_undo());
    CHECK_FALSE(ctx.stack().can_redo());

    ctx.stack().undo();
    CHECK(roughness(ctx.document(), "cube") == doctest::Approx(0.5));
    CHECK(ctx.stack().can_redo());

    ctx.stack().redo();
    CHECK(roughness(ctx.document(), "cube") == doctest::Approx(0.8));
}

TEST_CASE("coalescing collapses a drag into a single undo entry") {
    Context ctx;
    ctx.document().add(make_obj("cube", "Cube"));

    ctx.stack().begin_coalescing();
    for (double v : {0.6, 0.7, 0.85, 0.9}) {
        ctx.run(std::make_unique<SetRoughness>("cube", v));
    }
    ctx.stack().end_coalescing();

    CHECK(ctx.stack().size() == 1);
    CHECK(roughness(ctx.document(), "cube") == doctest::Approx(0.9));

    ctx.stack().undo();  // one undo reverts the whole drag
    CHECK(roughness(ctx.document(), "cube") == doctest::Approx(0.5));
    CHECK_FALSE(ctx.stack().can_undo());
}

TEST_CASE("running a new command discards the redo tail") {
    Context ctx;
    ctx.document().add(make_obj("cube", "Cube"));
    ctx.run(std::make_unique<SetRoughness>("cube", 0.8));
    ctx.stack().undo();
    REQUIRE(ctx.stack().can_redo());

    ctx.run(std::make_unique<SetRoughness>("cube", 0.3));
    CHECK_FALSE(ctx.stack().can_redo());  // branch discarded the redo
    CHECK(roughness(ctx.document(), "cube") == doctest::Approx(0.3));
}

TEST_CASE("selection tracks membership and the active element") {
    Selection sel;
    int changes = 0;
    sel.set_changed_handler([&] { ++changes; });

    sel.select("a");
    CHECK(sel.active() == "a");
    sel.add("b");
    CHECK(sel.size() == 2);
    CHECK(sel.active() == "b");  // last added is active
    sel.toggle("b");             // remove
    CHECK_FALSE(sel.contains("b"));
    CHECK(sel.active() == "a");  // active falls back
    sel.clear();
    CHECK(sel.empty());
    CHECK(changes == 4);
}

TEST_CASE("standard commands register with selection-aware state") {
    Context ctx;
    Localizer::instance().add_locale("en", standard_command_labels_en());
    register_standard_commands(ctx);
    ctx.document().add(make_obj("cube", "Cube"));
    ctx.document().add(make_obj("lamp", "Lamp"));

    // Delete disabled with empty selection, enabled once something is selected.
    CHECK_FALSE(ctx.registry().state(cmd::delete_, ctx).enabled);
    ctx.selection().select("cube");
    CHECK(ctx.registry().state(cmd::delete_, ctx).enabled);

    // Run delete: object removed and undoable.
    CHECK(ctx.run(cmd::delete_));
    CHECK(ctx.document().find("cube") == nullptr);
    ctx.stack().undo();
    CHECK(ctx.document().find("cube") != nullptr);
}

TEST_CASE("undo/redo command state reflects the stack") {
    Context ctx;
    register_standard_commands(ctx);
    ctx.document().add(make_obj("cube", "Cube"));

    CHECK_FALSE(ctx.registry().state(cmd::undo, ctx).enabled);
    ctx.run(std::make_unique<SetRoughness>("cube", 0.8));
    CHECK(ctx.registry().state(cmd::undo, ctx).enabled);
    CHECK_FALSE(ctx.registry().state(cmd::redo, ctx).enabled);
    ctx.stack().undo();
    CHECK(ctx.registry().state(cmd::redo, ctx).enabled);
}

TEST_CASE("keymap resolves chords with the user layer overriding default") {
    affineui::Keymap km;
    register_standard_keys(km);

    affineui::Chord ctrl_z{affineui::Key::Z, true, false, false, false};
    CHECK(km.command_for(ctrl_z) == cmd::undo);
    CHECK(km.shortcut_text(cmd::undo) == "Ctrl Z");
    CHECK(km.shortcut_text(cmd::duplicate) == "Ctrl D");

    // A user rebinding wins over the default.
    km.bind(ctrl_z, "custom.thing", affineui::Keymap::Layer::User);
    CHECK(km.command_for(ctrl_z) == "custom.thing");
}

TEST_CASE("localization falls back and echoes missing keys") {
    Localizer loc;
    loc.add_locale("en", {{"menu.file", "File"}});
    loc.add_locale("fr", {{"menu.file", "Fichier"}});
    loc.set_fallback_locale("en");

    CHECK(loc.text("menu.file") == "File");
    loc.set_locale("fr");
    CHECK(loc.text("menu.file") == "Fichier");
    CHECK(loc.text("menu.edit") == "menu.edit");  // missing -> echo key
}

namespace {
// A tool that records the gestures it receives, to test the router.
struct ProbeTool final : Tool {
    int clicks{0}, begins{0}, updates{0}, ends{0};
    affineui::Point last_delta{};
    [[nodiscard]] std::string_view id() const override { return "probe"; }
    void on_click(Context&, affineui::Point) override { ++clicks; }
    void on_drag_begin(Context&, const Drag&) override { ++begins; }
    void on_drag_update(Context&, const Drag& d) override {
        ++updates;
        last_delta = d.delta;
    }
    void on_drag_end(Context&, const Drag&) override { ++ends; }
};

affineui::Event mouse(affineui::EventType type, int x, int y) {
    affineui::Event ev;
    ev.type = type;
    ev.button = affineui::MouseButton::Left;
    ev.pos = {x, y};
    return ev;
}
}  // namespace

TEST_CASE("gesture router classifies a click vs a drag by threshold") {
    Context ctx;
    GestureRouter router(ctx);
    auto tool = std::make_shared<ProbeTool>();
    router.set_tool(tool);

    // A press + release without crossing the threshold is a click.
    router.on_pointer(mouse(affineui::EventType::MouseDown, 10, 10));
    router.on_pointer(mouse(affineui::EventType::MouseMove, 11, 11));
    router.on_pointer(mouse(affineui::EventType::MouseUp, 11, 11));
    CHECK(tool->clicks == 1);
    CHECK(tool->begins == 0);

    // A press + move past threshold + release is a drag.
    router.on_pointer(mouse(affineui::EventType::MouseDown, 50, 50));
    router.on_pointer(mouse(affineui::EventType::MouseMove, 80, 50));
    router.on_pointer(mouse(affineui::EventType::MouseUp, 80, 50));
    CHECK(tool->begins == 1);
    CHECK(tool->ends == 1);
    CHECK(tool->clicks == 1);  // unchanged — not a click
    CHECK(tool->last_delta.x == 30);
}

// ── TOML + settings persistence ─────────────────────────────────────────────

TEST_CASE("tiny TOML round-trips scalars, arrays, and nested tables") {
    const char* src =
        "title = \"Scene\"\n"
        "count = 3\n"
        "scale = 1.5\n"
        "visible = true\n"
        "accents = [\"cyan\", \"green\"]\n"
        "\n"
        "[ui.dock.outliner]\n"
        "open = true\n"
        "width = 260\n";
    toml::Table t = toml::parse(src);

    CHECK(toml::find(t, "title")->as_string() == "Scene");
    CHECK(toml::find(t, "count")->as_int() == 3);
    CHECK(toml::find(t, "scale")->as_double() == doctest::Approx(1.5));
    CHECK(toml::find(t, "visible")->as_bool());
    const auto* arr = toml::find(t, "accents")->as_array();
    REQUIRE(arr != nullptr);
    CHECK(arr->size() == 2);
    CHECK((*arr)[1].as_string() == "green");
    CHECK(toml::find(t, "ui.dock.outliner.open")->as_bool());
    CHECK(toml::find(t, "ui.dock.outliner.width")->as_int() == 260);

    // Dump then re-parse: values survive the round trip.
    toml::Table t2 = toml::parse(toml::dump(t));
    CHECK(toml::find(t2, "title")->as_string() == "Scene");
    CHECK(toml::find(t2, "ui.dock.outliner.width")->as_int() == 260);
}

TEST_CASE("malformed TOML lines are skipped, not fatal") {
    toml::Table t = toml::parse("good = 1\nthis is broken\n# comment\nalso = 2\n");
    CHECK(toml::find(t, "good")->as_int() == 1);
    CHECK(toml::find(t, "also")->as_int() == 2);
    CHECK(toml::find(t, "this is broken") == nullptr);
}

TEST_CASE("Preferences and Workspace are independent typed stores") {
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path();
    const std::string prefs_path = (dir / "affineui_test_prefs.toml").string();
    const std::string ws_path = (dir / "affineui_test_ws.toml").string();
    std::remove(prefs_path.c_str());
    std::remove(ws_path.c_str());

    Preferences prefs;
    prefs.set_string("theme", "decius");
    prefs.set_string("ui.accent", "cyan");
    prefs.set_bool("confirm_quit", true);
    REQUIRE(prefs.save(prefs_path));

    Workspace ws;
    ws.set_panel_open("outliner", true);
    ws.set_panel_size("outliner", 260);
    ws.set_string("last_tool", "move");
    REQUIRE(ws.save(ws_path));

    // Reload into fresh stores.
    Preferences prefs2;
    REQUIRE(prefs2.load(prefs_path));
    CHECK(prefs2.get_string("theme") == "decius");
    CHECK(prefs2.get_string("ui.accent") == "cyan");
    CHECK(prefs2.get_bool("confirm_quit"));

    Workspace ws2;
    REQUIRE(ws2.load(ws_path));
    CHECK(ws2.panel_open("outliner"));
    CHECK(ws2.panel_size("outliner", 0) == 260);
    CHECK(ws2.get_string("last_tool") == "move");

    // The two stores don't bleed into each other.
    CHECK_FALSE(prefs2.has("last_tool"));
    CHECK_FALSE(ws2.has("theme"));

    std::remove(prefs_path.c_str());
    std::remove(ws_path.c_str());
}

TEST_CASE("loading a missing settings file is safe and leaves the store usable") {
    Preferences prefs;
    CHECK_FALSE(prefs.load("definitely/not/a/real/path.toml"));
    CHECK(prefs.get_string("anything", "fallback") == "fallback");
    prefs.set_int("x", 1);  // still usable
    CHECK(prefs.get_int("x") == 1);
}
