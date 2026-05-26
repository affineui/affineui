// decius_game_editor - dense game/editor UI using Decius CSS.

#include "decius_interactions.h"

#include <affineui/affineui.h>

#include <sokol_log.h>

#include <sstream>
#include <string>
#include <unordered_map>

namespace dcs = demo::decius;

namespace {

struct GameEditorState {
    int  selected{1};
    bool playing{false};
    bool snap{true};
    bool cast_shadows{true};
    bool gpu_skinning{true};
    std::unordered_map<std::string, float> values{
        {"roughness", .62f},
    };
};

float value(const GameEditorState& s, std::string_view id, float fallback) {
    auto it = s.values.find(std::string(id));
    return it == s.values.end() ? fallback : it->second;
}

std::string tree_row(int idx, std::string_view icon, std::string_view name,
                     std::string_view meta, bool selected) {
    std::ostringstream h;
    h << "<div id=\"object-" << idx << "\" class=\"dcs-tree__row\"";
    if (selected) h << " aria-selected=\"true\"";
    h << "><span class=\"dcs-tree__chevron\"></span>" << dcs::icon(icon)
      << "<span class=\"dcs-tree__label\">" << demo::html_escape(name)
      << "</span><span class=\"dcs-tree__meta\">" << meta << "</span></div>";
    return h.str();
}

std::string property_row(std::string_view label, std::string_view value) {
    std::ostringstream h;
    h << "<div class=\"prop-row\"><span>" << demo::html_escape(label)
      << "</span><input class=\"dcs-input\" value=\"" << demo::html_escape(value)
      << "\"></div>";
    return h.str();
}

std::string render(const GameEditorState& s) {
    const char* selected_name = s.selected == 0 ? "WorldRoot" :
                                s.selected == 1 ? "Hero_mesh_high" :
                                s.selected == 2 ? "KeyLight" : "SplineRig";
    std::ostringstream h;
    h << R"HTML(
<!doctype html><html><head><meta charset="utf-8">
<link rel="stylesheet" href="frameworks/css/decius-css-0.4.1.bundle.min.css">
<style>
body{margin:0;background:#14161c}
.app{height:100vh;background:var(--dcs-bg-app);display:flex;flex-direction:column;overflow:hidden}
.center{flex:1;min-height:0;display:flex;gap:1px;background:var(--dcs-line)}
.left{flex:0 0 260px}.right{flex:0 0 340px}.main{flex:1;min-width:0}
.viewport-grid{height:100%;position:relative;background:linear-gradient(90deg,rgba(255,255,255,.05) 1px,transparent 1px),linear-gradient(rgba(255,255,255,.05) 1px,transparent 1px),radial-gradient(circle at 52% 42%,#4b5368,#171a22 72%);background-size:24px 24px,24px 24px,auto}
.gizmo{position:absolute;left:46%;top:35%;width:180px;height:120px;border:1px solid rgba(77,159,255,.75);box-shadow:0 0 0 1px rgba(77,159,255,.18),0 12px 32px rgba(0,0,0,.45)}
.gizmo:before{content:"";position:absolute;left:42px;top:-26px;width:94px;height:170px;border:1px dashed rgba(255,255,255,.25)}
.gizmo-axis{position:absolute;background:var(--dcs-accent)}.gx{left:-30px;top:58px;width:240px;height:2px}.gy{left:88px;top:-38px;width:2px;height:196px}
.prop-row{display:flex;align-items:center;gap:8px;margin-bottom:8px}.prop-row span{flex:0 0 92px;color:var(--dcs-text-mute);font-size:var(--dcs-fs-xs);text-transform:uppercase;letter-spacing:.08em}.prop-row .dcs-input{flex:1}
.asset-strip{display:flex;gap:8px;padding:10px;overflow:hidden}.asset{width:88px;height:68px;background:var(--dcs-well);border:1px solid var(--dcs-line-soft);border-radius:var(--dcs-r-2);display:flex;flex-direction:column;align-items:center;justify-content:center;gap:4px}
.asset i{font-size:20px;color:var(--dcs-accent)}
</style></head>
<body class="dcs" data-dcs-density="compact" data-dcs-style="3d">
<div class="app">
  <div class="dcs-menubar">
    <div class="dcs-menubar__brand">)HTML" << dcs::icon("decius") << R"HTML( Decius Forge</div>
    <button class="dcs-menubar__item">File</button><button class="dcs-menubar__item">Edit</button><button class="dcs-menubar__item">Build</button>
    <div class="dcs-menubar__spacer"></div>
    <div class="dcs-menubar__meta">Scene: ruins_arena_03</div>
  </div>
  <div class="center">
    <aside class="left dcs-dockpane">
      <div class="dcs-dockpane__tabs"><button class="dcs-dockpane__tab" aria-selected="true">)HTML" << dcs::icon("folder") << R"HTML( Hierarchy</button></div>
      <div class="dcs-dockpane__body">
        <div class="dcs-tree">)HTML"
      << tree_row(0, "cube", "WorldRoot", "12", s.selected == 0)
      << tree_row(1, "sphere", "Hero_mesh_high", "42k", s.selected == 1)
      << tree_row(2, "light", "KeyLight", "8k", s.selected == 2)
      << tree_row(3, "spline", "SplineRig", "16", s.selected == 3)
      << R"HTML(</div>
      </div>
    </aside>
    <main class="main dcs-dockpane">
      <div class="dcs-dockpane__tabs">
        <button class="dcs-dockpane__tab" aria-selected="true">)HTML" << dcs::icon("view-lit") << R"HTML( Lit View</button>
        <button class="dcs-dockpane__tab">)HTML" << dcs::icon("view-wire") << R"HTML( Wire</button>
        <div class="dcs-dockpane__tools">)HTML"
      << dcs::icon_button("select", "", true, "dcs-btn--sm")
      << dcs::icon_button("move", "", false, "dcs-btn--sm")
      << dcs::icon_button("rotate", "", false, "dcs-btn--sm")
      << dcs::icon_button("scale", "", false, "dcs-btn--sm")
      << R"HTML(</div>
      </div>
      <div class="dcs-dockpane__body dcs-viewport">
        <div class="viewport-grid"><div class="gizmo"><div class="gizmo-axis gx"></div><div class="gizmo-axis gy"></div></div></div>
        <div class="dcs-viewport__overlay dcs-viewport__overlay--tl"><div class="dcs-viewport__floater">)HTML"
      << dcs::button(s.playing ? "Pause" : "Play", s.playing ? "pause" : "play", "dcs-btn--primary", "play")
      << dcs::icon_button("snap", "snap", s.snap)
      << dcs::icon_button("grid", "", true)
      << R"HTML(</div></div>
      </div>
    </main>
    <aside class="right dcs-dockpane">
      <div class="dcs-dockpane__tabs"><button class="dcs-dockpane__tab" aria-selected="true">)HTML" << dcs::icon("cog") << R"HTML( Inspector</button></div>
      <div class="dcs-dockpane__body" style="padding:12px">
        <h3 style="margin:0 0 12px;font-size:16px">)HTML" << selected_name << R"HTML(</h3>)HTML"
      << property_row("Name", selected_name)
      << property_row("Position", "12.0, 4.2, -8.5")
      << property_row("Rotation", "0, 45, 0")
      << property_row("Material", "M_hero_worn")
      << "<div style=\"margin:14px 0\">" << dcs::slider(0, 1, value(s, "roughness", .62f), false, true, "roughness") << "</div>"
      << dcs::check("Cast shadows", s.cast_shadows, false, "cast-shadows")
      << dcs::toggle("GPU skinning", s.gpu_skinning, "gpu-skinning")
      << R"HTML(
      </div>
    </aside>
  </div>
  <div class="dcs-dockpane" style="height:118px;flex:0 0 118px">
    <div class="dcs-dockpane__tabs"><button class="dcs-dockpane__tab" aria-selected="true">)HTML" << dcs::icon("image") << R"HTML( Assets</button></div>
    <div class="asset-strip">
      <div class="asset">)HTML" << dcs::icon("mesh") << R"HTML(<span>Hero</span></div>
      <div class="asset">)HTML" << dcs::icon("texture") << R"HTML(<span>Albedo</span></div>
      <div class="asset">)HTML" << dcs::icon("curve") << R"HTML(<span>Rig</span></div>
      <div class="asset">)HTML" << dcs::icon("camera") << R"HTML(<span>Shot A</span></div>
    </div>
  </div>
  <div class="dcs-statusbar"><div class="dcs-statusbar__item dcs-statusbar__item--ok">READY</div><div class="dcs-statusbar__sep"></div><div class="dcs-statusbar__item">60.0 FPS</div><div class="dcs-statusbar__spacer"></div><div class="dcs-statusbar__item dcs-statusbar__item--accent">GPU 42%</div></div>
</div>
</body></html>)HTML";
    return h.str();
}

}  // namespace

int main() {
    affineui::Ui ui;
    demo::install_resource_loader(ui);
    GameEditorState state;
    auto rerender = [&] { ui.html(render(state)); ui.mark_dirty(); };
    rerender();

    ui.on_click("#play", [&] { state.playing = !state.playing; rerender(); });
    ui.on_click("#snap", [&] { state.snap = !state.snap; rerender(); });
    ui.on_click("#cast-shadows", [&] { state.cast_shadows = !state.cast_shadows; rerender(); });
    ui.on_click("#gpu-skinning", [&] { state.gpu_skinning = !state.gpu_skinning; rerender(); });
    ui.on_click("#object-0", [&] { state.selected = 0; rerender(); });
    ui.on_click("#object-1", [&] { state.selected = 1; rerender(); });
    ui.on_click("#object-2", [&] { state.selected = 2; rerender(); });
    ui.on_click("#object-3", [&] { state.selected = 3; rerender(); });
    dcs::install_value_interactions(ui, {
        [&](std::string_view id) { return value(state, id, 0.0f); },
        [&](std::string_view id, float v) { state.values[std::string(id)] = v; },
        rerender,
    });

    sapp_desc desc{};
    desc.width = 1440;
    desc.height = 900;
    desc.window_title = "AffineUI - Decius game editor";
    desc.high_dpi = true;
    desc.swap_interval = 0;
    desc.sample_count = 1;
    desc.logger.func = slog_func;
    affineui::sokol::wire(desc, ui, true);
    sapp_run(&desc);
    return 0;
}
