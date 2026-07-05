"""App stylesheet for the Decius Photo Edit sample.

Ported from the web reference app.css (§10 of the decode) on top of the
decius.css framework bundle that the Decius view theme loads.
"""

PHOTO_CSS = r"""
/* ── Shell ─────────────────────────────────────────────────────────────── */
.aui-root>.ps-app{margin:-24px;height:100vh;min-height:100vh;width:calc(100% + 48px)}
.ps-app{position:relative;display:flex;flex-direction:column;min-height:100vh;background:var(--dcs-bg-app,#1f222a);color:var(--dcs-text,#e7e9ee);overflow:hidden}
.ps-menubar{flex:0 0 auto;min-height:var(--dcs-h-lg,32px);height:var(--dcs-h-lg,32px);overflow:visible}
.ps-brand{display:inline-flex;align-items:center;align-self:stretch;gap:7px;padding:0 12px;margin:0 8px 0 0;background:#0d0f14;color:#e7e9ee;line-height:1}
.ps-brand__mark{color:var(--dcs-accent,#4f86d6);font-size:14px}
.ps-brand__name{font-weight:700;font-size:12px;white-space:nowrap}
.ps-doc-name{color:var(--dcs-text-mute,#8c93a3);font-family:var(--dcs-font-mono,monospace);font-size:12px}
.ps-svg{display:inline-flex;align-items:center;justify-content:center;width:16px;height:16px}

/* ── Options bar ───────────────────────────────────────────────────────── */
.ps-options{display:flex;align-items:center;gap:9px;height:var(--dcs-h-xl,38px);padding:0 10px;background:var(--dcs-surface-1,#252a34);border-bottom:1px solid var(--dcs-line,#343946);overflow:hidden;white-space:nowrap;flex:0 0 auto}
.ps-tool-glyph{display:inline-flex;align-items:center;justify-content:center;width:26px;height:26px;border-radius:3px;background:var(--dcs-well,#171a21);color:var(--dcs-accent,#4f86d6);font-size:16px;flex:0 0 auto}
.ps-tool-name{font-weight:700;font-size:13px}
.ps-opt-slot{display:flex;align-items:center;gap:8px;min-width:0;flex:1 1 auto;overflow:hidden}
.ps-opt-slot>.dcs-field{height:28px;min-height:28px;flex:0 0 auto}
.ps-opt-slot>.dcs-field>.dcs-field__label{flex:0 0 auto;font-size:11px}
.ps-opt-slot>.dcs-field>.dcs-slider{min-width:78px;width:120px}
.ps-opt-slot>.dcs-field>.aui-select{min-width:104px}
.ps-opt-slot .ps-opt-num input,.ps-opt-num .aui-input{width:58px}
.ps-opt-slot .dcs-field__label:empty{display:none}
.ps-opt-note{margin:0;color:var(--dcs-text-dim,#a6adbb);font-size:12px}
.ps-menubar .dcs-menubar__item{text-transform:none}

/* ── Body / stage ──────────────────────────────────────────────────────── */
.ps-body{position:relative;flex:1 1 auto;min-height:0;overflow:hidden;background:var(--dcs-stage,#151820)}
.ps-doc-dock{position:absolute;inset:0}
.ps-doc-dock>.dcs-dockpane{position:absolute;inset:0;display:flex;flex-direction:column}
.ps-stagewrap{position:absolute;inset:0;background:var(--dcs-stage,#151820);overflow:hidden}
.ps-ruler-corner{position:absolute;left:0;top:0;width:18px;height:18px;background:var(--dcs-surface-1,#252a34);border-right:1px solid var(--dcs-line,#343946);border-bottom:1px solid var(--dcs-line,#343946);z-index:2}
.ps-ruler{position:absolute;background:var(--dcs-surface-1,#252a34);color:var(--dcs-text-mute,#8c93a3);font:8px var(--dcs-font-mono,monospace);overflow:hidden;z-index:1}
.ps-ruler--h{left:18px;right:0;top:0;height:18px;border-bottom:1px solid var(--dcs-line,#343946)}
.ps-ruler--v{left:0;top:18px;bottom:0;width:18px;border-right:1px solid var(--dcs-line,#343946)}
.ps-ruler-ticks{display:flex;height:100%;align-items:flex-end}
.ps-ruler-ticks span{flex:none;border-left:1px solid var(--dcs-line-strong,#4b5262);height:8px;padding-left:2px;overflow:hidden;white-space:nowrap}
.ps-ruler--v .ps-ruler-ticks{flex-direction:column;align-items:flex-start;width:100%}
.ps-ruler--v .ps-ruler-ticks span{border-left:0;border-top:1px solid var(--dcs-line-strong,#4b5262);width:8px;height:auto;padding-left:0;padding-top:2px}
.ps-stage{position:absolute;left:18px;right:0;top:18px;bottom:0;overflow:hidden;cursor:crosshair}
.ps-doc{position:absolute;left:50%;top:50%;transform:translate(-50%,-50%) translate(var(--px,0),var(--py,0)) scale(var(--z,.67));transform-origin:center;box-shadow:0 0 0 1px #0008,0 24px 60px rgba(0,0,0,.45);background-image:repeating-conic-gradient(#cfcfcf 0 90deg,#fff 90deg 180deg);background-size:16px 16px;overflow:hidden}
.ps-layer-canvas{position:absolute;inset:0}
.ps-layer-text{display:flex;flex-direction:column;align-items:center;justify-content:center;gap:14px;font-size:120px;color:#fff}
.ps-layer-title{font-weight:700;font-size:1em;line-height:1;text-shadow:0 4px 20px rgba(0,0,0,.5);letter-spacing:2px}
.ps-layer-subtitle{font-family:var(--dcs-font-mono,monospace);font-weight:500;font-size:.25em;color:rgba(255,255,255,.82)}
.ps-marquee{position:absolute;outline:1px dashed #fff;box-shadow:0 0 0 1px #000;pointer-events:none;animation:ps-ants .6s linear infinite}
@keyframes ps-ants{0%{outline-offset:0}100%{outline-offset:-4px}}
.ps-grid-overlay{position:absolute;inset:0;background-image:linear-gradient(rgba(255,255,255,.12) 1px,transparent 1px),linear-gradient(90deg,rgba(255,255,255,.12) 1px,transparent 1px);background-size:64px 64px;pointer-events:none;opacity:.5}
.ps-stage-badge{position:absolute;z-index:5}
.ps-stage-badge--bl{left:8px;bottom:8px}
.ps-stage-badge--tr{right:8px;top:8px}

/* ── Tool strip ────────────────────────────────────────────────────────── */
.ps-toolstrip{position:absolute;left:12px;top:12px;display:flex;flex-direction:column;align-items:center;gap:2px;max-height:calc(100% - 24px);overflow:auto;z-index:15}
.ps-toolgrid{display:flex;flex-wrap:wrap;width:69px;gap:1px}
.ps-tool{position:relative;width:34px;height:34px;display:flex;align-items:center;justify-content:center;border-radius:3px;color:var(--dcs-text-dim,#a6adbb);cursor:pointer;border:1px solid transparent;font-size:17px}
.ps-tool:hover{background:var(--dcs-surface-2,#303642);color:var(--dcs-text,#e7e9ee)}
.ps-tool[aria-pressed=true]{background:var(--dcs-accent-dim,#263f64);color:var(--dcs-accent-hi,#b9d5ff);border-color:var(--dcs-accent-lo,#3b6ba8)}
.ps-tool[data-group=true]::after{content:"";position:absolute;right:3px;bottom:3px;border-left:4px solid transparent;border-bottom:4px solid var(--dcs-text-mute,#8c93a3)}
.ps-toolsep{width:28px;height:1px;background:var(--dcs-line-soft,#303642);margin:5px auto;flex:0 0 auto}
.ps-colorchips{position:relative;width:38px;height:38px;margin:8px 0 2px}
.ps-colorchip{position:absolute;width:24px;height:24px;border:1px solid var(--dcs-line-strong,#4b5262);border-radius:3px;box-shadow:0 3px 8px rgba(0,0,0,.32);cursor:pointer}
.ps-colorchip--bg{right:0;bottom:0;z-index:1}
.ps-colorchip--fg{left:0;top:0;z-index:2}
.ps-chip-mini{position:absolute;color:var(--dcs-text-mute,#8c93a3);font-size:11px;cursor:pointer;line-height:1}
.ps-chip-mini:hover{color:var(--dcs-text,#e7e9ee)}
.ps-swap{right:0;top:-2px}
.ps-reset{left:-1px;bottom:-1px}

/* ── Floating panels ───────────────────────────────────────────────────── */
.ps-floating{position:absolute;z-index:12}
.ps-float-panel{display:flex;flex-direction:column;min-height:90px}
/* NOTE: do not make this body a flex column — a flex-column overflow:auto
   body inside a top+bottom-anchored absolute panel trips a layout blowup in
   the native engine (content height ~2^30). Block body + height:100% child
   is equivalent here. */
.ps-float-panel>.dcs-panel__body{flex:1 1 auto;min-height:0;overflow:auto}
.ps-panel-title{display:flex;align-items:center;gap:7px;min-width:0}
.ps-panel-title span{overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
.ps-ptabs{display:flex;align-items:center;gap:2px;flex:1 1 auto;min-width:0}
.ps-ptab{display:inline-flex;align-items:center;gap:6px;height:24px;padding:0 9px;border-radius:3px;color:var(--dcs-text-dim,#a6adbb);font-size:12px;cursor:pointer;white-space:nowrap}
.ps-ptab:hover{background:var(--dcs-surface-2,#303642);color:var(--dcs-text,#e7e9ee)}
.ps-ptab[aria-selected=true]{background:var(--dcs-surface-2,#303642);color:var(--dcs-text,#e7e9ee)}

/* ── Navigator ─────────────────────────────────────────────────────────── */
.ps-nav-body{display:flex;flex-direction:column;gap:8px;padding:10px}
.ps-nav-thumb{position:relative;height:116px;border:1px solid var(--dcs-line,#343946);border-radius:3px;overflow:hidden;background-image:repeating-conic-gradient(#cfcfcf 0 90deg,#fff 90deg 180deg);background-size:10px 10px;cursor:move}
.ps-nav-doc{position:absolute;overflow:hidden;background:#0b1437}
.ps-nav-view{position:absolute;border:1.5px solid var(--dcs-danger,#ff5b6a);box-shadow:0 0 0 1px #0008;pointer-events:none}
.ps-nav-zoomrow{display:flex;align-items:center;gap:7px;color:var(--dcs-text-dim,#a6adbb);font-size:13px}
.ps-nav-zoomrow>.dcs-field{flex:1 1 auto;min-width:0;height:22px;min-height:22px}
.ps-nav-zoomrow i{cursor:pointer}
.ps-nav-zoomrow i:hover{color:var(--dcs-text,#e7e9ee)}
.ps-nav-pct{font-family:var(--dcs-font-mono,monospace);font-size:11px;min-width:34px;text-align:right}

/* ── Color / swatches panel ────────────────────────────────────────────── */
.ps-colorpanel{display:flex;flex-direction:column;gap:8px;padding:10px}
.ps-sv{height:120px;border-radius:3px;border:1px solid var(--dcs-line,#343946);position:relative}
.ps-sv-dot{position:absolute;width:12px;height:12px;border-radius:50%;border:2px solid #fff;box-shadow:0 0 0 1px #0009;transform:translate(-50%,-50%)}
.ps-hue{height:14px;border-radius:3px;border:1px solid var(--dcs-line,#343946);background:linear-gradient(90deg,red,#ff0,lime,cyan,blue,magenta,red);position:relative}
.ps-hue-dot{position:absolute;top:50%;width:5px;height:20px;border-radius:2px;background:#fff;box-shadow:0 0 0 1px #0009;transform:translate(-50%,-50%)}
.ps-rgb-row{display:flex;gap:6px}
.ps-rgb-row>.dcs-field{flex:1 1 0;min-width:0}
.ps-rgb-row input,.ps-rgb-row .aui-input{width:100%;min-width:0}
.ps-hex-field input,.ps-hex-field .aui-input{font-family:var(--dcs-font-mono,monospace)}
.ps-swatches{display:grid;grid-template-columns:repeat(10,1fr);gap:3px;padding:10px}
.ps-swatch-chip{aspect-ratio:1;min-height:20px;border-radius:3px;border:1px solid var(--dcs-line,#343946);cursor:pointer}
.ps-swatch-chip:hover{outline:1px solid var(--dcs-accent,#4f86d6);outline-offset:1px}

/* ── Layers panel ──────────────────────────────────────────────────────── */
.ps-layers{display:flex;flex-direction:column;min-height:0;height:100%}
.ps-layer-filter,.ps-layer-bo,.ps-layer-lock-row,.ps-layer-footer{display:flex;align-items:center;gap:3px;padding:6px 8px;border-bottom:1px solid var(--dcs-line-soft,#303642)}
.ps-layer-footer{border-top:1px solid var(--dcs-line,#343946);border-bottom:0;margin-top:auto}
.ps-layer-filter>.dcs-field,.ps-layer-bo>.dcs-field{height:24px;min-height:24px}
.ps-layer-filter .dcs-field__label:empty,.ps-layer-bo .dcs-field__label:empty{display:none}
.ps-layer-kind .aui-select{width:74px;min-width:74px}
.ps-blend-select .aui-select{width:108px;min-width:108px}
.ps-row-spacer{flex:1 1 auto}
.ps-amt-label{font-size:11px;color:var(--dcs-text-dim,#a6adbb);white-space:nowrap}
.ps-amt-field{height:22px;min-height:22px}
.ps-amt-field input,.ps-amt-field .aui-input{width:44px;font-family:var(--dcs-font-mono,monospace);text-align:right;padding:0 4px}
.ps-amt-field .dcs-field__label:empty{display:none}
.ps-tcap{font-weight:700;font-size:11px}
.ps-fx{font-style:italic;font-family:Georgia,serif}
.ps-layer-list{overflow:auto;min-height:112px;flex:1 1 auto}
.ps-layer{display:flex;align-items:center;gap:8px;padding:6px 8px;border-bottom:1px solid var(--dcs-line,#343946);cursor:pointer;min-height:48px}
.ps-layer:hover{background:var(--dcs-surface-1,#252a34)}
.ps-layer.is-active{background:var(--dcs-accent-dim,#263f64);box-shadow:inset 2px 0 0 var(--dcs-accent,#4f86d6)}
.ps-layer-eye{width:18px;text-align:center;color:var(--dcs-text-dim,#a6adbb);font-size:13px;flex:none}
.ps-layer-eye.is-off{opacity:.35}
.ps-layer-thumb{position:relative;width:34px;height:34px;flex:none;border:1px solid var(--dcs-line-strong,#4b5262);border-radius:3px;background-image:repeating-conic-gradient(#cfcfcf 0 90deg,#fff 90deg 180deg);background-size:8px 8px;overflow:hidden}
.ps-layer-thumb-fill{position:absolute;inset:0}
.ps-layer-name{flex:1;min-width:0;overflow:hidden;text-overflow:ellipsis;white-space:nowrap;font-size:12px}
.ps-layer-rename{flex:1;min-width:0;height:22px;min-height:22px}
.ps-layer-rename .dcs-field__label:empty{display:none}
.ps-layer-rename input,.ps-layer-rename .aui-input{width:100%;min-width:0;font-size:12px}
.ps-layer-lock{color:var(--dcs-text-mute,#8c93a3);font-size:12px;flex:none}
.ps-layer-actions{display:flex;gap:1px;flex:none}
.ps-layer-actions .dcs-btn{width:20px;height:20px;min-width:20px;padding:0;font-size:11px}

/* ── Adjustments / history / floatbar ──────────────────────────────────── */
.ps-adjust-grid{display:grid;grid-template-columns:repeat(6,1fr);gap:5px;padding:10px}
.ps-adjust{display:flex;align-items:center;justify-content:center;height:30px;border-radius:3px;color:var(--dcs-text-dim,#a6adbb);font-size:15px;cursor:pointer;border:1px solid transparent}
.ps-adjust:hover{background:var(--dcs-surface-2,#303642);color:var(--dcs-text,#e7e9ee);border-color:var(--dcs-line,#343946)}
.ps-history-item{display:flex;align-items:center;gap:8px;padding:7px 9px;border-bottom:1px solid var(--dcs-line,#343946);color:var(--dcs-text-dim,#a6adbb);cursor:pointer;font-size:12px}
.ps-history-item.is-current{background:var(--dcs-accent-dim,#263f64);color:var(--dcs-text,#e7e9ee)}
.ps-history-item.is-future{opacity:.4}
.ps-floatbar{position:absolute;left:50%;bottom:18px;transform:translateX(-50%);display:flex;align-items:center;gap:2px;z-index:16}

/* ── Statusbar ─────────────────────────────────────────────────────────── */
.ps-statusbar{display:flex;align-items:center;gap:8px;min-height:28px;padding:0 8px;background:var(--dcs-surface-1,#252a34);border-top:1px solid var(--dcs-line,#343946);font-size:12px;flex:0 0 auto}
.ps-statusbar p{margin:0}
.ps-status-spacer{flex:1}
.ps-zoom-wrap{height:22px;min-height:22px}
.ps-zoom-wrap .dcs-field__label:empty{display:none}
.ps-zoom-wrap input,.ps-zoom-wrap .aui-input{width:62px;text-align:center}

/* ── Static panel tabs (channels / paths / comps) ──────────────────────── */
.ps-list-row{display:flex;align-items:center;gap:8px;padding:7px 9px;border-bottom:1px solid var(--dcs-line,#343946);color:var(--dcs-text-dim,#a6adbb);font-size:12px}
.ps-list-row.is-active{background:var(--dcs-accent-dim,#263f64);color:var(--dcs-text,#e7e9ee)}
.ps-list-row .ps-row-meta{margin-left:auto;font-family:var(--dcs-font-mono,monospace);font-size:11px;color:var(--dcs-text-mute,#8c93a3)}
.ps-panel-note{margin:0;padding:12px;color:var(--dcs-text-mute,#8c93a3);font-size:12px;line-height:1.5}

/* ── Theme tweaks popover ──────────────────────────────────────────────── */
.ps-tweaks{position:absolute;right:8px;top:38px;width:248px;z-index:600;display:flex;flex-direction:column}
.ps-tweaks .dcs-panel__body{display:flex;flex-direction:column;gap:10px;padding:12px}
.ps-accent-dots{display:flex;gap:8px}
.ps-accent-dot{width:20px;height:20px;border-radius:50%;cursor:pointer;border:2px solid transparent}
.ps-accent-dot.is-active{border-color:#fff;box-shadow:0 0 0 1px #0009}

/* ── Dialogs ───────────────────────────────────────────────────────────── */
.ps-dialog-backdrop{position:fixed;inset:0;display:flex;align-items:center;justify-content:center;background:rgba(0,0,0,.45);z-index:500}
.ps-dialog{max-width:calc(100vw - 48px)}
.ps-dialog>.dcs-panel__header{display:flex;align-items:center;gap:8px}
.ps-dialog .ps-dlg-close{margin-left:auto;cursor:pointer;color:var(--dcs-text-mute,#8c93a3)}
.ps-dialog .ps-dlg-close:hover{color:var(--dcs-text,#e7e9ee)}
.ps-dlg-body{display:flex;flex-direction:column;gap:9px;padding:14px}
.ps-dlg-sec{margin:4px 0 0;font-size:11px;font-weight:700;letter-spacing:.06em;text-transform:uppercase;color:var(--dcs-text-mute,#8c93a3)}
.ps-dlg-static{margin:0;color:var(--dcs-text-dim,#a6adbb);font-size:12px;line-height:1.45}
.ps-dlg-body>.dcs-field>.dcs-field__label{flex:0 0 116px}
.ps-dialog-actions{display:flex;justify-content:flex-end;gap:8px;padding:10px 14px;border-top:1px solid var(--dcs-line,#343946)}
.ps-anchor-grid{display:grid;grid-template-columns:repeat(3,26px);gap:4px}
.ps-anchor{width:26px;height:26px;border-radius:3px;border:1px solid var(--dcs-line,#343946);background:var(--dcs-surface-1,#252a34);cursor:pointer}
.ps-anchor[aria-pressed=true]{background:var(--dcs-accent-dim,#263f64);border-color:var(--dcs-accent,#4f86d6)}
.ps-anchor-row{display:flex;align-items:flex-start;gap:12px}
.ps-anchor-label{flex:0 0 104px;font-size:12px;color:var(--dcs-text-dim,#a6adbb)}
.ps-kbd-list{display:flex;flex-wrap:wrap;gap:6px 16px}
.ps-kbd-list p{width:178px;margin:0;display:flex;justify-content:space-between;color:var(--dcs-text-dim,#a6adbb);font-size:12px}
.ps-badge-row{display:flex;flex-wrap:wrap;gap:6px}
"""
