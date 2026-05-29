// decius_dender - native C++ parity sample for the Decius DENDER web app.
//
// This intentionally mixes two layers:
//   - View widgets for application state and callbacks.
//   - trusted raw Decius markup for chrome that is still DOM/data-API shaped.

#include <affineui/affineui.h>

#include <cstdio>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

std::string native_css() {
    return R"CSS(
.dn-app{position:fixed;inset:0;display:flex;flex-direction:column;background:var(--dcs-bg-app);color:var(--dcs-text);overflow:hidden}
.dn-topbar{display:flex;align-items:center;gap:var(--dcs-s-2);min-height:var(--dcs-h-lg);padding:0 var(--dcs-s-2);box-sizing:border-box}
.dn-topbar>.dcs-menubar{height:auto;align-self:stretch;background:transparent;border:0;padding:0}
.dn-logo{display:inline-flex;align-items:center;align-self:stretch;gap:var(--dcs-s-2);padding:var(--dcs-text-nudge) var(--dcs-s-4) 0;background:#0d0f14;color:var(--dcs-text);line-height:1}
.dn-logo__mark{font-size:14px;color:var(--dcs-accent)}.dn-logo__name{font-weight:700;letter-spacing:.14em;font-size:var(--dcs-fs-sm)}
.dn-workarea{display:flex;flex:1 1 auto;min-height:0;background:var(--dcs-line)}
.dn-mainrow{display:flex;flex:1 1 auto;min-height:0;min-width:0}
.dn-viewport,.dn-outliner,.dn-inspector,.dn-timeline{min-width:0;min-height:0}
.dn-viewport{flex:1 1 auto}.dn-outliner{flex:0 0 260px}.dn-inspector{flex:0 0 320px}.dn-timeline{flex:0 0 132px}
.dn-viewport .dcs-dockpane__body{padding:0;display:flex;position:relative;overflow:hidden}
.dn-vp-canvas{position:relative;flex:1 1 auto;min-width:0;min-height:0;overflow:hidden;background:radial-gradient(120% 90% at 50% 8%,#50545d 0%,#3a3d45 38%,#292b31 78%,#202228 100%)}
.dn-vp-grid{position:absolute;inset:0;background-image:linear-gradient(rgba(255,255,255,.045) 1px,transparent 1px),linear-gradient(90deg,rgba(255,255,255,.045) 1px,transparent 1px);background-size:32px 32px;transform:skewY(-10deg) scale(1.1);opacity:.65}
.dn-cube{position:absolute;left:44%;top:36%;width:112px;height:112px;border:2px solid var(--dcs-accent);background:linear-gradient(135deg,#777d88,#444853);box-shadow:0 22px 50px rgba(0,0,0,.45),inset 0 1px 0 rgba(255,255,255,.12);transform:rotateX(58deg) rotateZ(45deg)}
.dn-vp-stats{position:absolute;left:56px;top:10px;color:#d7dae1;font-size:var(--dcs-fs-xs);line-height:1.5;text-shadow:0 1px 2px rgba(0,0,0,.8);pointer-events:none}
.dn-vp-corner{position:absolute;left:10px;bottom:8px;color:var(--dcs-text-dim);font-size:var(--dcs-fs-xs);text-shadow:0 1px 2px rgba(0,0,0,.8);pointer-events:none}
.dn-toolrail{position:absolute;left:8px;top:8px;z-index:5;gap:2px}
.dn-npanel{position:absolute;right:8px;top:8px;bottom:8px;width:220px;z-index:4}
.dn-npanel .dcs-dockpane__body{padding:var(--dcs-s-3);overflow:auto}
.dn-outliner .dcs-dockpane__body,.dn-inspector .dcs-dockpane__body{padding:var(--dcs-s-3);overflow:auto}
.dn-inspector .dcs-field{min-width:0}.dn-inspector .dcs-btn-row{gap:var(--dcs-s-2)}
.dn-timeline .dcs-dockpane__body{padding:0;overflow:hidden}
.dn-timeline-track{position:relative;height:100%;background:linear-gradient(180deg,var(--dcs-bg),var(--dcs-well))}
.dn-timeline-ruler{height:28px;border-bottom:1px solid var(--dcs-line);background:repeating-linear-gradient(90deg,var(--dcs-surface-2) 0 1px,transparent 1px 44px);color:var(--dcs-text-mute);font-size:var(--dcs-fs-xs);padding:6px 8px;box-sizing:border-box}
.dn-playhead{position:absolute;left:34%;top:0;bottom:0;width:2px;background:var(--dcs-accent);box-shadow:0 0 8px var(--dcs-accent)}
.dn-key{position:absolute;top:58px;width:8px;height:8px;background:var(--dcs-warn);transform:rotate(45deg)}
@media (max-width:900px){.dn-inspector{flex-basis:260px}.dn-outliner{display:none}.dn-logo__name{display:none}}
)CSS";
}

std::string menu_markup() {
    return R"HTML(
<div class="dcs-menu" id="dn-menu-file" hidden>
  <div class="dcs-menu__label">File</div>
  <div class="dcs-menu__item" data-dcs-value="new"><span class="dcs-menu__icon"><i class="di di-file"></i></span><span class="dcs-menu__label-text">New</span><span class="dcs-menu__shortcut">Ctrl N</span></div>
  <div class="dcs-menu__item" data-dcs-value="open"><span class="dcs-menu__icon"><i class="di di-folder-open"></i></span><span class="dcs-menu__label-text">Open</span><span class="dcs-menu__shortcut">Ctrl O</span></div>
  <div class="dcs-menu__sep"></div>
  <div class="dcs-menu__item" data-dcs-value="save"><span class="dcs-menu__icon"><i class="di di-save"></i></span><span class="dcs-menu__label-text">Save</span><span class="dcs-menu__shortcut">Ctrl S</span></div>
</div>
<div class="dcs-menu" id="dn-menu-edit" hidden>
  <div class="dcs-menu__item"><span class="dcs-menu__icon"><i class="di di-undo"></i></span><span class="dcs-menu__label-text">Undo</span></div>
  <div class="dcs-menu__item"><span class="dcs-menu__icon"><i class="di di-redo"></i></span><span class="dcs-menu__label-text">Redo</span></div>
  <div class="dcs-menu__sep"></div>
  <div class="dcs-menu__item"><span class="dcs-menu__label-text">Duplicate Objects</span></div>
</div>
<div class="dcs-menu" id="dn-menu-render" hidden>
  <div class="dcs-menu__item"><span class="dcs-menu__icon"><i class="di di-render"></i></span><span class="dcs-menu__label-text">Render Image</span><span class="dcs-menu__shortcut">F12</span></div>
  <div class="dcs-menu__item"><span class="dcs-menu__label-text">Render Animation</span></div>
</div>
<div class="dcs-menu" id="dn-menu-workspace" hidden>
  <div class="dcs-menu__item" data-dcs-value="layout"><span class="dcs-menu__check"><i class="di di-check"></i></span><span class="dcs-menu__label-text">Layout</span></div>
  <div class="dcs-menu__item" data-dcs-value="modeling"><span class="dcs-menu__label-text">Modeling</span></div>
  <div class="dcs-menu__item" data-dcs-value="shading"><span class="dcs-menu__label-text">Shading</span></div>
  <div class="dcs-menu__item" data-dcs-value="animation"><span class="dcs-menu__label-text">Animation</span></div>
</div>
<div class="dcs-menu" id="dn-menu-mode" hidden>
  <div class="dcs-menu__item"><span class="dcs-menu__icon"><i class="di di-cube"></i></span><span class="dcs-menu__label-text">Object Mode</span></div>
  <div class="dcs-menu__item"><span class="dcs-menu__icon"><i class="di di-mesh"></i></span><span class="dcs-menu__label-text">Edit Mode</span></div>
  <div class="dcs-menu__item"><span class="dcs-menu__label-text">Sculpt Mode</span></div>
</div>
<div class="dcs-menu" id="dn-menu-engine" hidden>
  <div class="dcs-menu__item" data-dcs-value="eevee"><span class="dcs-menu__label-text">Eevee</span></div>
  <div class="dcs-menu__item" data-dcs-value="workbench"><span class="dcs-menu__label-text">Workbench</span></div>
  <div class="dcs-menu__item" data-dcs-value="cycles"><span class="dcs-menu__label-text">Cycles</span><span class="dcs-menu__check"><i class="di di-check"></i></span></div>
</div>
)HTML";
}

class DenderApp {
public:
    DenderApp()
        : app_(make_config()) {}

    int run() {
        reload();
        return app_.run();
    }

private:
    void reload() {
        auto view = build_view();
        app_.load_view(view);
        app_.set_stylesheet(native_css());
    }

    static affineui::App::Config make_config() {
        affineui::App::Config cfg;
        cfg.title = "AffineUI - DENDER native C++";
        cfg.width = 1280;
        cfg.height = 820;
        cfg.clear_color = affineui::Color{31, 34, 42, 255};
        cfg.high_dpi = true;
        cfg.asset_folders = {"examples"};
        return cfg;
    }

    affineui::View build_view() {
        affineui::View view{affineui::ViewTheme::Decius};
        view.selector(affineui::decius::selector::style, affineui::decius::style::flat);
        view.selector(affineui::decius::selector::density, affineui::decius::density::comfortable);
        view.selector(affineui::decius::selector::accent, "orange");

        view.begin();
        {
            auto app = view.container("dn-app", "dender");
            build_topbar(view);
            build_workarea(view);
            build_statusbar(view);
            view.html(menu_markup(), "dender-menus");
        }
        view.end();
        return view;
    }

    void build_topbar(affineui::View& view) {
        auto top = view.container("dcs-toolbar dn-topbar", "topbar");
        view.html(R"HTML(
<div class="dn-logo"><i class="di di-decius dn-logo__mark"></i><span class="dn-logo__name">DENDER</span></div>
<nav class="dcs-menubar">
  <button class="dcs-menubar__item" data-dcs-toggle="menu" data-dcs-target="#dn-menu-file">File</button>
  <button class="dcs-menubar__item" data-dcs-toggle="menu" data-dcs-target="#dn-menu-edit">Edit</button>
  <button class="dcs-menubar__item" data-dcs-toggle="menu" data-dcs-target="#dn-menu-render">Render</button>
</nav>
<span class="dcs-toolbar__sep"></span>
<button class="dcs-select dcs-select--btn" data-dcs-toggle="menu" data-dcs-target="#dn-menu-workspace"><span class="dcs-select__label">Layout</span><span class="dcs-select__caret"><i class="di di-chevron-down"></i></span></button>
<span class="dcs-toolbar__spacer"></span>
<button class="dcs-btn dcs-btn--icon dcs-btn--ghost"><i class="di di-save"></i></button>
<button class="dcs-btn dcs-btn--icon dcs-btn--ghost"><i class="di di-cog"></i></button>
)HTML", "topbar-markup");
    }

    void build_workarea(affineui::View& view) {
        auto work = view.container("dcs-dock dcs-dock--v dn-workarea", "workarea");
        {
            auto row = view.container("dn-mainrow", "main-row");
            build_outliner(view);
            view.html(R"(<div class="dcs-splitter" data-dcs-splitter></div>)",
                      "split-left-center");
            build_viewport(view);
            view.html(R"(<div class="dcs-splitter" data-dcs-splitter></div>)",
                      "split-center-right");
            build_inspector(view);
        }
        view.html(R"(<div class="dcs-splitter dcs-splitter--h" data-dcs-splitter="h"></div>)",
                  "split-main-timeline");
        build_timeline(view);
    }

    void build_outliner(affineui::View& view) {
        auto pane = view.container("dcs-dockpane dn-outliner", "outliner");
        view.html(R"HTML(
<div class="dcs-dockpane__tabbar">
  <div class="dcs-dockpane__tabs">
    <button class="dcs-dockpane__tab" aria-selected="true" data-dcs-target="#dn-outliner-body"><i class="di di-folder-open"></i> Outliner</button>
  </div>
  <div class="dcs-dockpane__toolbars">
    <div class="dcs-dockpane__toolbar"><input class="dcs-input" value="" placeholder="Search" style="max-width:128px"><button class="dcs-btn dcs-btn--icon dcs-btn--ghost"><i class="di di-eq"></i></button></div>
  </div>
</div>
)HTML", "outliner-tabbar");
        {
            auto body = view.container("dcs-dockpane__body", "outliner-body");
            auto tree = view.container("dcs-tree", "outliner-tree");
            tree.attr("data-dcs-select", "multi");
            tree_row(view, "Scene", "Scene", 0, true);
            tree_row(view, "Collection", "Collection", 1, true);
            tree_row(view, "Cube", "Cube", 2, false);
            tree_row(view, "Camera", "Camera", 2, false);
            tree_row(view, "Key Light", "Key Light", 2, false);
            tree_row(view, "World", "World", 0, false);
        }
    }

    void build_viewport(affineui::View& view) {
        auto pane = view.container("dcs-dockpane dcs-dockpane--center dn-viewport", "viewport");
        view.html(R"HTML(
<div class="dcs-dockpane__tabbar">
  <div class="dcs-dockpane__tabs">
    <button class="dcs-dockpane__tab" aria-selected="true" data-dcs-target="#dn-vp-body"><i class="di di-cube"></i> 3D Viewport</button>
  </div>
  <div class="dcs-dockpane__toolbars">
    <div class="dcs-dockpane__toolbar">
      <button class="dcs-select dcs-select--btn" data-dcs-toggle="menu" data-dcs-target="#dn-menu-mode"><span class="dcs-select__label">Object Mode</span><span class="dcs-select__caret"><i class="di di-chevron-down"></i></span></button>
      <span class="dcs-toolbar__sep"></span>
      <button class="dcs-menubar__item">View</button><button class="dcs-menubar__item">Select</button><button class="dcs-menubar__item">Add</button>
      <span class="dcs-toolbar__spacer"></span>
      <button class="dcs-btn dcs-btn--icon dcs-btn--ghost" aria-pressed="true"><i class="di di-gizmo"></i></button>
      <button class="dcs-btn dcs-btn--icon dcs-btn--ghost" aria-pressed="true"><i class="di di-view-bbox"></i></button>
      <button class="dcs-select dcs-select--btn" data-dcs-toggle="menu" data-dcs-target="#dn-menu-engine"><span class="dcs-select__label">Cycles</span><span class="dcs-select__caret"><i class="di di-chevron-down"></i></span></button>
    </div>
  </div>
</div>
<div class="dcs-dockpane__body">
  <div id="dn-vp-body" class="dn-vp-canvas" data-dcs-tabpanel data-dcs-float-host>
    <div class="dn-vp-grid"></div>
    <div class="dn-cube"></div>
    <div class="dn-vp-stats"><b>User Perspective</b><br>(1) Collection | Cube</div>
    <div class="dn-vp-corner">Verts 8 | Faces 6 | Tris 12 | Objects 3/3</div>
    <div class="dcs-toolbar dcs-toolbar--v dcs-toolbar--sm dcs-toolbar--floating dn-toolrail" data-dcs-drag-bounds=".dn-vp-canvas">
      <span class="dcs-grip dcs-grip--h" data-dcs-drag-handle></span>
      <button class="dcs-btn dcs-btn--icon dcs-btn--sm dcs-btn--ghost" aria-pressed="true" data-dcs-radio="tool"><i class="di di-cross-target"></i></button>
      <button class="dcs-btn dcs-btn--icon dcs-btn--sm dcs-btn--ghost" data-dcs-radio="tool"><i class="di di-move"></i></button>
      <button class="dcs-btn dcs-btn--icon dcs-btn--sm dcs-btn--ghost" data-dcs-radio="tool"><i class="di di-rotate"></i></button>
      <button class="dcs-btn dcs-btn--icon dcs-btn--sm dcs-btn--ghost" data-dcs-radio="tool"><i class="di di-scale"></i></button>
    </div>
    <div class="dcs-panel dcs-panel--floating dn-npanel" data-dcs-drag-bounds=".dn-vp-canvas">
      <div class="dcs-dockpane">
        <div class="dcs-dockpane__tabbar" data-dcs-drag-handle><div class="dcs-dockpane__tabs"><button class="dcs-dockpane__tab" aria-selected="true" data-dcs-target="#dn-npanel-item">Item</button><button class="dcs-dockpane__tab" data-dcs-target="#dn-npanel-tool">Tool</button></div></div>
        <div class="dcs-dockpane__body"><div id="dn-npanel-item" data-dcs-tabpanel><div class="dcs-props"><div class="dcs-field"><span class="dcs-field__label">Lens</span><div data-dcs-combo data-value="50" data-min="1" data-max="250" data-step="0.5" data-suffix=" mm"></div></div><div class="dcs-field"><span class="dcs-field__label">Clip</span><div data-dcs-combo data-value="0.1" data-step="0.01"></div></div></div></div><div id="dn-npanel-tool" data-dcs-tabpanel hidden>Tool settings</div></div>
      </div>
    </div>
  </div>
</div>
)HTML", "viewport-markup");
    }

    void build_inspector(affineui::View& view) {
        auto pane = view.container("dcs-dockpane dn-inspector", "inspector");
        view.html(R"HTML(
<div class="dcs-dockpane__tabbar">
  <div class="dcs-dockpane__tabs">
    <button class="dcs-dockpane__tab" aria-selected="true" data-dcs-target="#dn-inspector-body"><i class="di di-cog"></i> Inspector</button>
  </div>
</div>
)HTML", "inspector-tabbar");
        {
            auto body = view.container("dcs-dockpane__body", "inspector-body");
            view.heading(2, selected_object_, "dcs-panel__title", "inspector-title");
            view.input("Name", selected_object_, "text", "object-name")
                .on_change([this](std::string_view value) {
                    selected_object_ = std::string(value);
                    reload();
                });
            view.input("Tint", "#e8843a", "color", "object-tint")
                .on_change([](std::string_view value) {
                    std::printf("Tint changed: %.*s\n",
                                static_cast<int>(value.size()), value.data());
                });
            view.slider("Roughness", 0.42, 0.0, 1.0, "roughness");
            view.input("Location X", "0.000", "number", "loc-x");
            view.input("Location Y", "0.000", "number", "loc-y");
            view.input("Location Z", "0.000", "number", "loc-z");
            view.dropdown("Modifier", {"Subdivision", "Bevel", "Mirror", "Solidify"},
                          "Subdivision", "modifier");
            view.button_group("Space", {"Local", "Global", "View"}, "Global", "space");
            view.checkbox("Show wireframe overlay", show_wireframe_, "show-wire")
                .on_change([this](std::string_view value) {
                    show_wireframe_ = value == "true" || value == "1" || value == "on";
                    reload();
                });
            {
                auto row = view.container("dcs-btn-row", "inspector-actions");
                view.button("Apply", true, "apply").on_click([] {
                    std::puts("Apply clicked");
                });
                view.button("Reset", false, "reset").on_click([this] {
                    selected_object_ = "Cube";
                    reload();
                });
            }
        }
    }

    void build_timeline(affineui::View& view) {
        auto pane = view.container("dcs-dockpane dn-timeline", "timeline");
        view.html(R"HTML(
<div class="dcs-dockpane__tabbar">
  <div class="dcs-dockpane__tabs"><button class="dcs-dockpane__tab" aria-selected="true" data-dcs-target="#dn-timeline-body"><i class="di di-timeline"></i> Timeline</button></div>
  <div class="dcs-dockpane__toolbars"><div class="dcs-dockpane__toolbar"><button class="dcs-btn dcs-btn--icon dcs-btn--ghost"><i class="di di-play"></i></button><button class="dcs-btn dcs-btn--icon dcs-btn--ghost"><i class="di di-keyframe"></i></button></div></div>
</div>
<div class="dcs-dockpane__body"><div id="dn-timeline-body" class="dn-timeline-track" data-dcs-tabpanel><div class="dn-timeline-ruler">1  24  48  72  96  120</div><div class="dn-playhead"></div><span class="dn-key" style="left:18%"></span><span class="dn-key" style="left:34%"></span><span class="dn-key" style="left:70%"></span></div></div>
)HTML", "timeline-markup");
    }

    void build_statusbar(affineui::View& view) {
        view.html(R"HTML(
<div class="dcs-statusbar">
  <span class="dcs-statusbar__item dcs-statusbar__item--ok"><i class="di di-check-circle"></i> Ready</span>
  <span class="dcs-statusbar__item">native C++ sample</span>
  <span class="dcs-statusbar__spacer"></span>
  <span class="dcs-statusbar__item dcs-statusbar__item--accent">menus, splitters, floating toolbar, widgets</span>
</div>
)HTML", "statusbar");
    }

    void tree_row(affineui::View& view,
                  std::string_view label,
                  std::string_view value,
                  int depth,
                  bool branch) {
        const bool selected = selected_object_ == value;
        auto row = view.button(label, selected, std::string{"outliner-"} + std::string(value));
        row.cls("dcs-tree__row")
           .attr("style", "width:100%;--depth:" + std::to_string(depth))
           .attr("aria-selected", selected ? "true" : "false");
        if (branch) row.attr("aria-expanded", "true");
        row.on_click([this, value = std::string(value)] {
            selected_object_ = value;
            reload();
        });
    }

    affineui::App app_;
    std::string selected_object_{"Cube"};
    bool show_wireframe_{true};
};

}  // namespace

int main() {
    DenderApp app;
    return app.run();
}
