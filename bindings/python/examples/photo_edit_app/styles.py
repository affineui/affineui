"""App stylesheet for the Decius Photo Edit sample.

Ported from the web reference app.css (§10 of the decode) on top of the
decius.css framework bundle. The Decius *view theme* only emits the
`.dcs-*` / `.di-*` class names — the framework CSS that styles them (and
the `di` icon font) is a separate bundle the app must load, exactly as the
C++ examples do via app::read_framework_bundle. `read_decius_bundle()`
below is the Python equivalent; app.py prepends it to PHOTO_CSS and hands
the app the bundle's directory as the stylesheet base URL so the bundle's
relative `url(../fonts/...)` resolve like a <link>ed sheet.
"""

from __future__ import annotations

from pathlib import Path

# The framework version the Decius view theme defaults to (src/app/view.cpp
# decius::default_version). Kept in one place so the bundle path matches
# what the view emits classes for.
DECIUS_VERSION = "0.6.2"


def read_decius_bundle() -> tuple[str, str]:
    """Return (css_text, base_url) for the Decius framework bundle.

    Resolves the bundle file with the same candidate order the C++ helper
    uses (beside the running script, then cwd-relative, then the repo
    layout), so it works whether run from bindings/python, the repo root,
    or a packaged copy. Returns ("", "") if not found — the app still runs,
    just unstyled, which is the visible symptom this loader fixes.

    base_url is the bundle directory (with a trailing slash): per CSS
    semantics the bundle's `url(../fonts/decius-icons.woff2)` resolves
    against the sheet's own location.
    """
    href = f"frameworks/css/decius-css-{DECIUS_VERSION}.bundle.min.css"
    here = Path(__file__).resolve().parent           # …/photo_edit_app
    examples_dir = here.parent                        # …/examples
    candidates = [
        examples_dir / href,                          # packaged beside examples/
        Path(href),                                   # cwd-relative
        Path("examples") / href,                      # repo/bindings/python
        examples_dir.parent.parent.parent / "examples" / href,  # repo root
    ]
    for path in candidates:
        try:
            css = path.read_text(encoding="utf-8")
        except (OSError, UnicodeDecodeError):
            continue
        if css:
            base = path.parent.as_posix()
            if not base.endswith("/"):
                base += "/"
            return css, base
    return "", ""


PHOTO_CSS = r"""
/* ── Shell ─────────────────────────────────────────────────────────────── */
.aui-root>.ps-app{margin:-24px;height:100vh;min-height:100vh;width:calc(100% + 48px)}
.ps-app{position:relative;display:flex;flex-direction:column;min-height:100vh;background:var(--dcs-bg-app,#1f222a);color:var(--dcs-text,#e7e9ee);overflow:hidden}
/* The framework menubar has horizontal padding; the brand block should sit
   flush against the left edge (its dark background runs to the corner), so
   drop the left padding — otherwise there's a gap before the logo. */
.ps-menubar{flex:0 0 auto;min-height:var(--dcs-h-lg,32px);height:var(--dcs-h-lg,32px);overflow:visible;padding-left:0}
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
/* Flex column so the document_view workarea (flex:1 1 0;height:0 host)
   fills the body; the tool strip / floatbar are absolute overlays. */
.ps-body{position:relative;display:flex;flex-direction:column;flex:1 1 auto;min-height:0;overflow:hidden;background:var(--dcs-stage,#151820)}
.ps-stagewrap{position:absolute;inset:0;background:var(--dcs-stage,#151820);overflow:hidden}
.ps-ruler-corner{position:absolute;left:0;top:0;width:18px;height:18px;background:var(--dcs-surface-1,#252a34);border-right:1px solid var(--dcs-line,#343946);border-bottom:1px solid var(--dcs-line,#343946);z-index:2}
.ps-ruler{position:absolute;background:var(--dcs-surface-1,#252a34);color:var(--dcs-text-mute,#8c93a3);font:8px var(--dcs-font-mono,monospace);overflow:hidden;z-index:1}
.ps-ruler--h{left:18px;right:0;top:0;height:18px;border-bottom:1px solid var(--dcs-line,#343946)}
.ps-ruler--v{left:0;top:18px;bottom:0;width:18px;border-right:1px solid var(--dcs-line,#343946)}
.ps-ruler-ticks{position:relative;width:100%;height:100%}
.ps-ruler--h .ps-ruler-ticks span{position:absolute;top:0;height:100%;border-left:1px solid var(--dcs-line-strong,#4b5262);padding-left:2px;overflow:hidden;white-space:nowrap}
.ps-ruler--v .ps-ruler-ticks span{position:absolute;left:0;width:100%;border-top:1px solid var(--dcs-line-strong,#4b5262);padding-top:1px;overflow:hidden;white-space:nowrap}
.ps-stage{position:absolute;left:18px;right:0;top:18px;bottom:0;overflow:hidden;cursor:crosshair}
/* The raster core paints the zoomed/panned document (checkerboard,
   composite, pen preview) into this custom-paint canvas each frame. */
.ps-stage-canvas{position:absolute;inset:0}
.ps-marquee{position:absolute;outline:1px dashed #fff;box-shadow:0 0 0 1px #000;pointer-events:none;animation:ps-ants .6s linear infinite;z-index:900}
@keyframes ps-ants{0%{outline-offset:0}100%{outline-offset:-4px}}
.ps-grid-overlay{position:absolute;background-image:linear-gradient(rgba(255,255,255,.12) 1px,transparent 1px),linear-gradient(90deg,rgba(255,255,255,.12) 1px,transparent 1px);background-size:64px 64px;pointer-events:none;opacity:.5}
.ps-stage-badge{position:absolute;z-index:5}
.ps-stage-badge--bl{left:8px;bottom:8px}
.ps-stage-badge--tr{right:8px;top:8px}

/* ── Tool strip ────────────────────────────────────────────────────────── */
.ps-toolstrip{position:absolute;left:37px;top:47px;display:flex;flex-direction:column;align-items:center;gap:2px;max-height:calc(100% - 59px);overflow:auto;z-index:15}
/* Two-column tool strip built as explicit rows (ps-toolrow) of up to two
   tools, with each separator as its own full-width row. Deterministic — no
   reliance on flex-wrap orphan behavior or grid-column placement. */
.ps-toolgrid{display:flex;flex-direction:column;width:69px;gap:1px;align-items:center}
.ps-toolrow{display:flex;gap:1px;justify-content:flex-start;width:69px}
.ps-tool{position:relative;width:34px;height:34px;display:flex;align-items:center;justify-content:center;border-radius:3px;color:var(--dcs-text-dim,#a6adbb);cursor:pointer;border:1px solid transparent;font-size:17px}
.ps-tool:hover{background:var(--dcs-surface-2,#303642);color:var(--dcs-text,#e7e9ee)}
.ps-tool[aria-pressed=true]{background:var(--dcs-accent-dim,#263f64);color:var(--dcs-accent-hi,#b9d5ff);border-color:var(--dcs-accent-lo,#3b6ba8)}
/* A selected tool keeps its blue icon even while hovered (hover must not
   repaint a pressed tool white). */
.ps-tool[aria-pressed=true]:hover{color:var(--dcs-accent-hi,#b9d5ff);background:var(--dcs-accent-dim,#263f64)}
/* Bottom-right corner nib on grouped tools: a small SVG triangle inside a
   CSS-sized wrapper. The wrapper's fixed 6x6 box (and overflow:hidden) keeps
   the SVG tiny in the corner — a bare inline <svg> otherwise stretches to
   fill the button. currentColor inherits the tool colour. */
.ps-tool-nib{position:absolute;right:2px;bottom:2px;width:6px;height:6px;overflow:hidden;color:var(--dcs-text-mute,#8c93a3);line-height:0}
.ps-tool-nib svg{display:block;width:6px;height:6px}
.ps-tool[aria-pressed=true] .ps-tool-nib{color:var(--dcs-accent-hi,#b9d5ff)}
.ps-toolsep{width:28px;height:1px;background:#4b5262;margin:5px 0}
.ps-colorchips{position:relative;width:38px;height:38px;margin:8px 0 2px}
.ps-colorchip{position:absolute;width:24px;height:24px;border:1px solid var(--dcs-line-strong,#4b5262);border-radius:3px;box-shadow:0 3px 8px rgba(0,0,0,.32);cursor:pointer}
.ps-colorchip--bg{right:0;bottom:0;z-index:1}
.ps-colorchip--fg{left:0;top:0;z-index:2}
/* The swap/reset markers hold an inline SVG, which stretches to fill its box
   unless the box is fixed-size and clipping (same as the tool nib). Give the
   mini chips an explicit box and size the SVG in CSS. */
/* Same look as the tool-strip buttons (icon colour + hover), but these are
   momentary actions, not toggles — no selected/blue state. */
.ps-chip-mini{position:absolute;width:14px;height:14px;display:flex;align-items:center;justify-content:center;overflow:hidden;color:var(--dcs-text-dim,#a6adbb);cursor:pointer;line-height:0;border-radius:3px;border:1px solid transparent}
.ps-chip-mini svg{display:block;width:11px;height:11px}
.ps-chip-mini:hover{background:var(--dcs-surface-2,#303642);color:var(--dcs-text,#e7e9ee)}
.ps-chip-mini:active{background:var(--dcs-surface-3,#3a4150)}
.ps-swap{right:-2px;top:-3px}
.ps-reset{left:-3px;bottom:-3px}

/* ── Floating panels ───────────────────────────────────────────────────── */
/* The palettes are DECLARED dockpanels (dcs-panel--floating > dcs-dockpane
   chrome emitted by the framework); only their tabpanel content is ours.
   The tabpanel fills the pane body and is the panel's SINGLE scroll region —
   panel content must NOT add its own nested scroll area, or you get two
   scrollbars (see .ps-layer-list, which is deliberately overflow:visible). */
.dcs-panel--floating .dcs-dockpane__body>[data-dcs-tabpanel]{height:100%;overflow:auto}

/* ── Navigator ─────────────────────────────────────────────────────────── */
.ps-nav-body{display:flex;flex-direction:column;gap:8px;padding:10px}
.ps-nav-thumb{position:relative;height:116px;border:1px solid var(--dcs-line,#343946);border-radius:3px;overflow:hidden;background:var(--dcs-well,#171a21);cursor:move}
.ps-nav-canvas{position:absolute;inset:0}
.ps-nav-zoomrow{display:flex;align-items:center;gap:7px;color:var(--dcs-text-dim,#a6adbb);font-size:13px}
.ps-nav-zoomrow>.dcs-field{flex:1 1 auto;min-width:0;height:22px;min-height:22px;display:flex;align-items:center}
/* The slider's labeled field has an empty label span — collapse it so the
   track fills the row instead of being shoved right. */
.ps-nav-zoomrow>.dcs-field>.dcs-field__label{display:none}
.ps-nav-zoomrow>.dcs-field>.dcs-slider{flex:1 1 auto;min-width:0}
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
.ps-rgb-field input{font-variant-numeric:tabular-nums lining-nums;text-align:right}
.ps-hex-field input,.ps-hex-field .aui-input{font-family:var(--dcs-font-mono,monospace)}
.ps-swatches{display:grid;grid-template-columns:repeat(10,1fr);gap:3px;padding:10px}
.ps-swatch-chip{aspect-ratio:1;min-height:20px;border-radius:3px;border:1px solid var(--dcs-line,#343946);cursor:pointer}
.ps-swatch-chip:hover{outline:1px solid var(--dcs-accent,#4f86d6);outline-offset:1px}

/* ── Layers panel ──────────────────────────────────────────────────────── */
/* Fill the dock body via flex (like the web), NOT height:100%. height:100%
   against a flex-basis'd body overflowed by a hair, triggering the body's own
   overflow:auto ON TOP of the inner .ps-layer-list scroll — two scrollbars.
   flex:1 + min-height:0 makes ps-layers fit exactly so only the list scrolls. */
/* Fills its tabpanel (which the floating-panel rule above makes a flex column
   that clips), so .ps-layer-list is the panel's single scroll region. */
.ps-layers{display:flex;flex-direction:column;min-height:0;flex:1 1 0}
/* The layers panel is a flex column whose list takes the slack; without an
   explicit basis these header/footer rows get squeezed when the panel is
   short. Pin them: never grow, never shrink, and keep a minimum height so the
   filter/blend controls stay legible. */
.ps-layer-filter,.ps-layer-bo,.ps-layer-lock-row,.ps-layer-footer{display:flex;align-items:center;gap:3px;padding:6px 8px;border-bottom:1px solid var(--dcs-line-soft,#303642);flex:0 0 auto;min-height:36px}
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
/* The list is the flexible region — it must be allowed to SHRINK (min-height:0),
   otherwise its minimum plus the pinned header/footer rows exceeds a short
   panel, the dock body starts scrolling too, and you get two scrollbars. */
/* NOT overflow:auto — the tabpanel above is this panel's single scroll region.
   A scroll here would nest inside it and show a second scrollbar. */
.ps-layer-list{overflow:visible;min-height:0;flex:1 1 auto}
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
/* Shared flat button style for both toolbars' momentary buttons: no border
   or fill at rest, a subtle surface highlight on hover, and a brighter grey
   while pressed (:active). These floatbar buttons are NOT toggles, so they
   never set aria-pressed and never take the blue selected state — that stays
   exclusive to the selectable tools. Overrides the framework dcs-btn chrome. */
.ps-toolbtn.dcs-btn{height:28px;min-height:28px;padding:0 8px;display:inline-flex;align-items:center;justify-content:center;border:1px solid transparent;border-radius:3px;background:transparent;color:var(--dcs-text-dim,#a6adbb);box-shadow:none}
/* Icon-only variants are square and unpadded. */
.ps-toolbtn.dcs-btn--icon{width:28px;padding:0}
/* Fit button: the four-corner frame mark beside its label. Size the SVG in
   CSS — a bare inline <svg> otherwise stretches to fill its box. */
.ps-fitbtn{gap:5px;white-space:nowrap}
.ps-fit-icon{display:block;width:14px;height:14px;flex:0 0 auto}
.ps-toolbtn.dcs-btn:hover{background:var(--dcs-surface-2,#303642);color:var(--dcs-text,#e7e9ee)}
.ps-toolbtn.dcs-btn:active{background:var(--dcs-surface-3,#3a4150);color:var(--dcs-text,#e7e9ee)}

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
/* Wide enough that the Density button group (Compact/Comfortable/Spacious)
   isn't clipped. */
.ps-tweaks{position:absolute;right:8px;top:38px;width:340px;z-index:600;display:flex;flex-direction:column}
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
