#include "game_editor_styles.h"

namespace ge {

std::string native_css() {
    // The small amount of app-custom CSS: the shell, the dock column sizing,
    // and the faux-3D viewport. Everything else is standard Decius. Mirrors the
    // reference index.html so the C++ app and the browser ground truth match.
    return R"CSS(
.ge-app{position:fixed;inset:0;display:flex;flex-direction:column;background:var(--dcs-bg-app);color:var(--dcs-text);overflow:hidden}

/* Center viewport (the document pane the dock engine marks .dcs-dockpane--center):
   its body must become a flex container so the canvas child fills it (and a
   torn-off floating panel positioned inside the float-host is sized / visible).
   Decius already gives the center body position:relative;overflow:hidden; we add
   the flex chain. The dock engine puts the content directly in the body (no
   separate tabpanel wrapper), so the canvas is a direct flex child. */
.dcs-dockpane--center > .dcs-dockpane__body{padding:0;display:flex}
.ge-vp-canvas{position:relative;flex:1 1 auto;min-width:0;min-height:0;overflow:hidden;
  background:radial-gradient(120% 90% at 50% 8%,#50545d 0%,#3a3d45 38%,#292b31 78%,#202228 100%)}
.ge-vp-grid{position:absolute;inset:0;opacity:.65;
  background-image:linear-gradient(rgba(255,255,255,.045) 1px,transparent 1px),
                   linear-gradient(90deg,rgba(255,255,255,.045) 1px,transparent 1px);
  background-size:32px 32px;transform:skewY(-10deg) scale(1.1)}
.ge-cube{position:absolute;left:calc(50% - 56px);top:calc(42% - 56px);width:112px;height:112px;
  border:2px solid var(--dcs-accent);background:linear-gradient(135deg,#777d88,#444853);
  box-shadow:0 22px 50px rgba(0,0,0,.45),inset 0 1px 0 rgba(255,255,255,.12);
  transform:rotateX(58deg) rotateZ(45deg)}
.ge-vp-stats{position:absolute;left:12px;top:10px;color:#d7dae1;font-size:var(--dcs-fs-xs);
  line-height:1.5;text-shadow:0 1px 2px rgba(0,0,0,.8);pointer-events:none}
.ge-toolrail{z-index:5}

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
