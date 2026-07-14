#include "dender_styles.h"

namespace dender {

std::string native_css() {
    // Port of the web sample's app.css (reference §7) on top of the Decius
    // bundle. Differences from the web are deliberate adaptations: the range
    // tint uses background+opacity (no color-mix here), the playhead bar is
    // the element itself (no ::before), the inspector rail/sheet anchor on
    // the engine-emitted #props-body tabpanel instead of a :has() rule, and
    // the viewport scene is the DenderViewport SVG layer (z-index 1, under
    // the z 4-6 overlays) instead of the web's WebGL canvas.
    return R"CSS(
.dn-app{position:fixed;inset:0;display:flex;flex-direction:column;background:var(--dcs-bg-app);color:var(--dcs-text);overflow:hidden}

/* ── Topbar ─────────────────────────────────────────────────────────────── */
.dn-topbar{gap:var(--dcs-s-2);padding-left:var(--dcs-s-2);align-items:center}
.dn-topbar>.dcs-menubar{height:auto;align-self:stretch;background:transparent;border-bottom:none;padding:0;line-height:1}
.dn-topbar>.dcs-menubar .dcs-menubar__item{align-self:stretch}

/* macOS, no system title bar: DENDER's TITLE STRIP is the topbar, not the
   menubar inside it — the logo sits to the menubar's left. So the topbar takes
   the window-button inset (AffineUI publishes the measured band as
   --affineui-titlebar-inset-left), and the menubar gives it back: Decius binds
   the inset to .dcs-menubar for the common case where the bar IS the strip, and
   here it isn't, so applying it twice would indent the menus by a second 70-odd
   pixels. The logo, sitting in the reserved band, is already black. */
:root[data-affineui-platform=macos][data-affineui-titlebar=hidden] .dn-topbar,
:root[data-affineui-platform=macos][data-affineui-titlebar=hidden-inset] .dn-topbar{
  border-left:var(--affineui-titlebar-inset-left,0px) solid #000;
  padding-left:0;
}
:root[data-affineui-platform=macos][data-affineui-titlebar=hidden] .dn-topbar>.dcs-menubar,
:root[data-affineui-platform=macos][data-affineui-titlebar=hidden-inset] .dn-topbar>.dcs-menubar{
  border-left:0;
  padding-left:0;
}
/* The logo block sits in the title area beside the window buttons, so it is
   spaced and aligned exactly like the game editor's brand: hard against the
   reserved band (no left padding — the band already provides the gap, and it is
   the same gap the buttons keep between themselves), roomier on the right, and
   riding 2px lower so its cap-height lines up with the buttons' centres rather
   than the box's. The 4px of top padding is what buys those 2px. */
:root[data-affineui-platform=macos][data-affineui-titlebar=hidden] .dn-logo,
:root[data-affineui-platform=macos][data-affineui-titlebar=hidden-inset] .dn-logo{
  margin-left:0;
  background:#000;
  /* Top 1px: DENDER's topbar is TALLER than a plain menubar (the workspace and
     scene pickers on the right set its height), so the logo needs far less of a
     nudge than the game editor's brand to sit level with the window buttons.
     Right s-7: the block has to breathe before the grey starts — at s-5 the
     final E was almost touching it. */
  padding:1px var(--dcs-s-7) 0 2px;
  align-self:stretch;   /* the black area covers the taller bar's full height */
}
.dn-logo{display:inline-flex;align-items:center;align-self:stretch;gap:var(--dcs-s-2);padding:var(--dcs-text-nudge) var(--dcs-s-4) 0;margin-left:calc(-1 * var(--dcs-s-2));background:#0d0f14;line-height:1}
.dn-logo:hover{background:#14171e}
.dn-logo__mark{font-size:14px;color:var(--dcs-accent)}
.dn-logo__name{font-weight:700;letter-spacing:.14em;font-size:var(--dcs-fs-sm);color:#e7e9ee}
.dn-engine-label{color:var(--dcs-text-mute);font-size:var(--dcs-fs-xs)}

/* ── Theme tweaks popover ───────────────────────────────────────────────── */
.dn-tweaks-popover{width:260px}
.dn-accent-row{display:flex;gap:var(--dcs-s-2);align-items:center}
.dn-accent-dot{width:14px;height:14px;border-radius:50%;border:0;padding:0;cursor:pointer;box-shadow:inset 0 0 0 1px rgba(255,255,255,.18)}
.dn-accent-dot[aria-pressed="true"]{box-shadow:0 0 0 2px var(--dcs-bg-app),0 0 0 3px currentColor}

/* ── Viewport canvas + overlay stack ────────────────────────────────────── */
.dcs-dockpane--center>.dcs-dockpane__body{padding:0;display:flex}
.dcs-dockpane--center>.dcs-dockpane__body>[data-dcs-tabpanel]{flex:1 1 auto;display:flex;min-width:0;min-height:0}
.dn-vp-canvas{position:relative;flex:1 1 auto;min-width:0;min-height:0;overflow:hidden;background:radial-gradient(120% 90% at 50% 8%,#50545d 0%,#3a3d45 38%,#292b31 78%,#202228 100%)}
.dn-vp-scenelayer{position:absolute;inset:0;z-index:1;overflow:hidden}
.dn-vp-stats{position:absolute;left:56px;top:var(--dcs-s-3);z-index:4;color:#d7dae1;font-size:var(--dcs-fs-xs);line-height:1.5;text-shadow:0 1px 2px rgba(0,0,0,.8);pointer-events:none}
.dn-vp-corner{position:absolute;left:var(--dcs-s-3);bottom:var(--dcs-s-3);z-index:4;color:var(--dcs-text-dim);font-size:var(--dcs-fs-xs);pointer-events:none}
.dn-toolrail{z-index:5;gap:2px}
.dn-toolrail .dcs-divider{width:60%;height:1px;background:var(--dcs-line);margin:2px auto}
.dn-navcluster{position:absolute;top:var(--dcs-s-3);right:232px;z-index:5;display:flex;gap:var(--dcs-s-3)}
.dn-gizmo{position:relative;width:72px;height:72px}
.dn-navbtns{display:flex;flex-direction:column;gap:2px}

/* ── Outliner ───────────────────────────────────────────────────────────── */
#outliner-body{height:100%;overflow:auto}

/* ── Inspector: left icon rail + sheet ──────────────────────────────────── */
#props-body{height:100%;display:flex;flex-direction:row;overflow:hidden;padding:0}
.dn-props-rail{background:var(--dcs-surface-1);padding:var(--dcs-s-2) var(--dcs-s-1);gap:1px;border-right:1px solid var(--dcs-line)}
.dn-props-rail .dcs-tab{width:28px;height:28px;background:transparent;border:0;border-radius:var(--dcs-r-1);color:var(--dcs-text-dim)}
.dn-props-rail .dcs-tab:hover{background:var(--dcs-surface-2);color:var(--dcs-text)}
.dn-props-rail .dcs-tab[aria-selected="true"]{background:var(--dcs-accent-dim);color:var(--dcs-accent)}
.dn-props__sheet{flex:1;min-width:0;overflow:auto}
.dn-props__sheet .dcs-foldout{padding:0 var(--dcs-s-3)}
.dn-props-search{display:flex;align-items:center;gap:var(--dcs-s-1)}
.dn-datablock{gap:var(--dcs-s-2);align-items:center;padding:var(--dcs-s-2) var(--dcs-s-3)}
.dn-datablock__icon{color:var(--dcs-accent)}
.dn-cap{font-size:var(--dcs-fs-xs);color:var(--dcs-text-mute);padding:var(--dcs-s-3);margin:0}
.dn-add-block{display:flex;align-items:center;justify-content:center;gap:var(--dcs-s-2);width:calc(100% - 2 * var(--dcs-s-3));margin:var(--dcs-s-3)}

/* ── Modifier card / material chip ──────────────────────────────────────── */
.dn-modifier{margin:var(--dcs-s-3);border-radius:var(--dcs-r-3);border:1px solid var(--dcs-line);background:var(--dcs-surface-1);overflow:hidden}
.dn-modifier__head{display:flex;align-items:center;gap:var(--dcs-s-2);height:var(--dcs-h);padding:0 var(--dcs-s-2);background:var(--dcs-surface-2);border-bottom:1px solid var(--dcs-line)}
.dn-modifier__title{flex:1;font-weight:500;font-size:var(--dcs-fs-sm)}
.dn-modifier__body{padding:var(--dcs-s-3);display:grid;gap:var(--dcs-s-3)}
.dn-mat-preview{width:18px;height:18px;border-radius:50%;background:conic-gradient(from 200deg,#cf6b3a,#e8943c 40%,#8a4a26);box-shadow:inset 0 2px 4px rgba(255,255,255,.35),inset 0 -3px 5px rgba(0,0,0,.4)}

/* ── Timeline dopesheet ─────────────────────────────────────────────────── */
[data-aui-name="pane-timeline"]{background:var(--dcs-bg)}
#timeline-body{height:100%;display:flex;flex-direction:column;overflow:hidden;padding:0}
.dn-timeline-body{position:relative;flex:1 1 auto;min-height:0;display:flex;flex-direction:column;background:var(--dcs-bg-app);overflow:hidden}
.dn-tl-range{position:absolute;top:0;bottom:0;background:var(--dcs-accent);opacity:.09;pointer-events:none}
.dn-tl-ruler{position:relative;flex:0 0 22px;height:22px;background:var(--dcs-surface-1);border-bottom:1px solid var(--dcs-line)}
.dn-tl-tick{position:absolute;bottom:0;width:1px;height:7px;background:var(--dcs-line)}
.dn-tl-tick--major{height:22px;background:var(--dcs-line-strong)}
.dn-tl-tick span{position:absolute;left:3px;top:2px;font-size:var(--dcs-fs-xs);color:var(--dcs-text-mute);font-variant-numeric:tabular-nums;white-space:nowrap}
.dn-tl-tracks{position:relative;flex:1 1 auto;min-height:0}
.dn-tl-track{position:relative;height:20px;border-bottom:1px solid var(--dcs-line)}
.dn-tl-track__label{position:absolute;left:var(--dcs-s-3);top:2px;font-size:var(--dcs-fs-xs);color:var(--dcs-text-dim)}
.dn-key{position:absolute;top:5px;width:9px;height:9px;margin-left:-4.5px;transform:rotate(45deg);background:var(--dcs-accent);border:1px solid #2a1c08;border-radius:1.5px}
.dn-playhead{position:absolute;top:0;bottom:0;width:2px;margin-left:-1px;background:var(--dcs-accent);z-index:6;pointer-events:none}
.dn-playhead__flag{position:absolute;top:0;left:-13px;width:28px;height:15px;background:var(--dcs-accent);color:#0a1220;font-size:10px;font-weight:700;border-radius:0 0 2px 2px;display:flex;align-items:center;justify-content:center;font-variant-numeric:tabular-nums}
)CSS";
}

}  // namespace dender
