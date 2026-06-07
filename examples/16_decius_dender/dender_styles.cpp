#include "dender_styles.h"

namespace dender {

std::string native_css() {
    return R"CSS(
.dn-app{position:fixed;inset:0;display:flex;flex-direction:column;background:var(--dcs-bg-app);color:var(--dcs-text);overflow:hidden}
.dn-topbar{display:flex;align-items:center;gap:var(--dcs-s-2);min-height:var(--dcs-h-lg);padding:0 var(--dcs-s-2);box-sizing:border-box}
.dn-topbar>.dcs-menubar{height:auto;align-self:stretch;background:transparent;border:0;padding:0}
.dn-app-title{display:inline-flex;align-items:center;align-self:stretch;gap:var(--dcs-s-3);min-width:0}
.dn-logo{display:inline-flex;align-items:center;align-self:stretch;gap:var(--dcs-s-2);padding:var(--dcs-text-nudge) var(--dcs-s-4) 0;background:#0d0f14;color:var(--dcs-text);line-height:1}
.dn-logo__mark{font-size:14px;color:var(--dcs-accent)}
.dn-logo__name{font-weight:700;letter-spacing:.14em;font-size:var(--dcs-fs-sm)}
.dn-document-title{max-width:220px;overflow:hidden;text-overflow:ellipsis;white-space:nowrap;color:var(--dcs-text-dim);font-size:var(--dcs-fs-sm)}
.dn-document-title__dirty{color:var(--dcs-warn);font-size:var(--dcs-fs-xs)}
.dn-workarea{display:flex;flex:1 1 auto;min-height:0;background:var(--dcs-line)}
.dn-mainrow{display:flex;flex:1 1 auto;min-height:0;min-width:0}
.dn-viewport,.dn-outliner,.dn-inspector,.dn-timeline{min-width:0;min-height:0}
.dn-viewport{flex:1 1 auto}
.dn-outliner{flex:0 0 260px}
.dn-inspector{flex:0 0 320px}
.dn-timeline{flex:0 0 132px}
.dn-viewport .dcs-dockpane__body{padding:0;display:flex;position:relative;overflow:hidden}
.dn-vp-canvas{position:relative;flex:1 1 auto;min-width:0;min-height:0;overflow:hidden;background:radial-gradient(120% 90% at 50% 8%,#50545d 0%,#3a3d45 38%,#292b31 78%,#202228 100%)}
.dn-vp-grid{position:absolute;inset:0;background-image:linear-gradient(rgba(255,255,255,.045) 1px,transparent 1px),linear-gradient(90deg,rgba(255,255,255,.045) 1px,transparent 1px);background-size:32px 32px;transform:skewY(-10deg) scale(1.1);opacity:.65}
.dn-vp-wire{position:absolute;left:calc(44% - 8px);top:calc(36% - 8px);width:128px;height:128px;border:1px dashed rgba(255,255,255,.28);transform:rotateX(58deg) rotateZ(45deg);pointer-events:none}
.dn-cube{position:absolute;width:112px;height:112px;border:2px solid var(--dcs-accent);background:linear-gradient(135deg,#777d88,#444853);box-shadow:0 22px 50px rgba(0,0,0,.45),inset 0 1px 0 rgba(255,255,255,.12);transform:rotateX(58deg) rotateZ(45deg)}
.dn-vp-stats{position:absolute;left:56px;top:10px;color:#d7dae1;font-size:var(--dcs-fs-xs);line-height:1.5;text-shadow:0 1px 2px rgba(0,0,0,.8);pointer-events:none}
.dn-vp-corner{position:absolute;left:10px;bottom:8px;color:var(--dcs-text-dim);font-size:var(--dcs-fs-xs);text-shadow:0 1px 2px rgba(0,0,0,.8);pointer-events:none}
.dn-toolrail{position:absolute;left:8px;top:8px;z-index:5;gap:2px}
.dn-npanel{position:absolute;right:8px;top:8px;bottom:8px;width:220px;z-index:4}
.dn-npanel .dcs-dockpane__body{padding:var(--dcs-s-3);overflow:auto}
.dn-outliner .dcs-dockpane__body,.dn-inspector .dcs-dockpane__body{padding:var(--dcs-s-3);overflow:auto}
.dn-inspector .dcs-field{min-width:0}
.dn-inspector .dcs-btn-row{gap:var(--dcs-s-2)}
.dn-list-panel{display:flex;flex-direction:column;gap:var(--dcs-s-2);margin-top:var(--dcs-s-3)}
.dn-list-panel__title{font-size:var(--dcs-fs-xs);font-weight:700;color:var(--dcs-text-mute);margin:0;text-transform:uppercase}
.dn-graph-readout{display:flex;flex-direction:column;gap:2px;margin-top:var(--dcs-s-3);padding-top:var(--dcs-s-3);border-top:1px solid var(--dcs-line)}
.dn-graph-row{display:flex;justify-content:space-between;gap:var(--dcs-s-2);font-size:var(--dcs-fs-xs);color:var(--dcs-text-dim)}
.dn-graph-row__value{color:var(--dcs-text)}
.dn-timeline .dcs-dockpane__body{padding:0;overflow:hidden}
.dn-timeline-track{position:relative;height:100%;background:linear-gradient(180deg,var(--dcs-bg),var(--dcs-well))}
.dn-timeline-ruler{height:28px;border-bottom:1px solid var(--dcs-line);background:repeating-linear-gradient(90deg,var(--dcs-surface-2) 0 1px,transparent 1px 44px);color:var(--dcs-text-mute);font-size:var(--dcs-fs-xs);padding:6px 8px;box-sizing:border-box}
.dn-playhead{position:absolute;left:34%;top:0;bottom:0;width:2px;background:var(--dcs-accent);box-shadow:0 0 8px var(--dcs-accent)}
.dn-key{position:absolute;top:58px;width:8px;height:8px;background:var(--dcs-warn);transform:rotate(45deg)}
.dn-modal{position:absolute;inset:0;display:flex;align-items:center;justify-content:center;background:rgba(0,0,0,.35);z-index:40}
.dn-modal__panel{width:360px}
.dn-notification{position:absolute;right:14px;bottom:32px;max-width:320px;padding:var(--dcs-s-3);border:1px solid var(--dcs-line);background:var(--dcs-bg);box-shadow:0 18px 50px rgba(0,0,0,.35);z-index:20}
.dn-notification--info{border-color:var(--dcs-accent)}
.dn-notification__title{font-size:var(--dcs-fs-sm);margin:0 0 var(--dcs-s-1)}
.dn-notification__body{margin:0;color:var(--dcs-text-dim);font-size:var(--dcs-fs-xs)}
@media (max-width:900px){.dn-inspector{flex-basis:260px}.dn-outliner{display:none}.dn-logo__name,.dn-document-title{display:none}}
)CSS";
}

}  // namespace dender
