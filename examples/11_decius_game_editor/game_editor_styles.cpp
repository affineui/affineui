#include "game_editor_styles.h"

namespace ge {

std::string native_css() {
    // The small amount of app-custom CSS: the shell, the dock column sizing,
    // and the faux-3D viewport. Everything else is standard Decius. Mirrors the
    // reference index.html so the C++ app and the browser ground truth match.
    return R"CSS(
.ge-app{position:fixed;inset:0;display:flex;flex-direction:column;background:var(--dcs-bg-app);color:var(--dcs-text);overflow:hidden}

/* Center viewport (the document pane the dock engine marks .dcs-dockpane--center):
   the flex chain body → tabpanel → canvas must stretch every link so the canvas
   fills the pane. The dock engine wraps pane content in a classless
   [data-dcs-tabpanel] div; without the tabpanel rule that wrapper is a plain
   block and the chain collapses (canvas gets a 0-height box, nothing paints).
   Same pattern as the DENDER web sample's app.css. */
.dcs-dockpane--center > .dcs-dockpane__body{padding:0;display:flex}
.dcs-dockpane--center > .dcs-dockpane__body > [data-dcs-tabpanel]{
  flex:1 1 auto;min-width:0;min-height:0;display:flex}
.ge-vp-canvas{position:relative;flex:1 1 auto;min-width:0;min-height:0;overflow:hidden;
  background:#232529}
/* The e3d render target: a custom-paint block filling the pane. */
.ge-vp-3dcanvas{position:absolute;left:0;top:0;width:100%;height:100%}
/* Navigation axis ball (painter-drawn orientation gizmo, web parity:
   top-right of the viewport, nub clicks snap the camera). */
.ge-vp-navball{position:absolute;top:10px;right:12px;width:72px;height:72px;z-index:5}
.ge-vp-stats{position:absolute;left:12px;top:10px;color:#d7dae1;font-size:var(--dcs-fs-xs);
  line-height:1.5;text-shadow:0 1px 2px rgba(0,0,0,.8);pointer-events:none}
.ge-toolrail{z-index:5}

.dcs-popover .dcs-popover__body{box-sizing:border-box;width:100%;align-items:stretch}
.dcs-popover .dcs-colorfield__picker,
.dcs-popover .dcs-color-square,
.dcs-popover .dcs-hue-bar{width:100%;align-self:stretch;box-sizing:border-box}

/* Asset strip. */
.ge-asset-strip{display:flex;gap:8px;padding:10px;overflow:auto}
.ge-asset{width:88px;height:64px;flex:0 0 auto;background:var(--dcs-well);
  border:1px solid var(--dcs-line-soft);border-radius:var(--dcs-r-2);
  display:flex;flex-direction:column;align-items:center;justify-content:center;gap:4px}
.ge-asset i{font-size:20px;color:var(--dcs-accent)}
.ge-asset__label{margin:0;font-size:var(--dcs-fs-xs);color:var(--dcs-text-dim)}
)CSS";
}

}  // namespace ge
