"""Decius CSS reference pages mirrored into the AffineUI component gallery."""

from textwrap import dedent


def page_specs():
    return (
        (
            "decius-foundations",
            "Foundations",
            "Colors, type, spacing, icons",
            "palette",
            build_foundations,
        ),
        (
            "decius-buttons",
            "Buttons & Toolbars",
            "Button intents, groups, and rails",
            "gizmo",
            build_buttons,
        ),
        (
            "decius-inputs",
            "Inputs & Forms",
            "Text, number, select, color, form and props",
            "edit",
            build_inputs,
        ),
        (
            "decius-ranges",
            "Ranges",
            "Sliders, faders, knobs, combo fields",
            "gain",
            build_ranges,
        ),
        (
            "decius-checks",
            "Checks & Switches",
            "Checks, radios, switches, segmented buttons",
            "check-circle",
            build_checks,
        ),
        (
            "decius-layout",
            "Panels & Foldouts",
            "Panels, tabs, subpanels, foldouts",
            "layers",
            build_layout,
        ),
        (
            "decius-dock",
            "Docking",
            "Dock panes, splitters, tearoffs, floating toolbars",
            "array",
            build_docking,
        ),
        (
            "decius-data",
            "Data Views",
            "Lists, trees, tables, badges, cards",
            "grid",
            build_data_views,
        ),
        (
            "decius-overlays",
            "Overlays",
            "Menus, popovers, toasts, context menus",
            "menu",
            build_overlays,
        ),
        (
            "decius-feedback",
            "Feedback",
            "Alerts, modals, status bars",
            "alert",
            build_feedback,
        ),
        # Editors and Hardware are DISABLED — listed under "Planned Coverage" in
        # component_gallery.py instead. Their surfaces don't render correctly
        # yet, and a gallery whose job is to show what works should not be
        # showing what doesn't. build_editors / build_hardware below are kept so
        # they can be re-enabled here once the surfaces are fixed.
    )


def _html(v, key, markup):
    v.html(dedent(markup).strip(), key=key)


def _note(ctx, v, key):
    v.paragraph(
        "These pages mirror the Decius CSS site groups. Where AffineUI has a "
        "first-class widget call, the page uses it; raw DOM fixtures cover the "
        "remaining Decius shapes so renderer and interaction gaps stay visible.",
        classes=ctx.note_class(),
        key=f"{key}-note",
    )


def _section(ctx, v, title, key, markup):
    ctx.section(
        v,
        title,
        "split",
        key,
        lambda body: _html(body, f"{key}-html", markup),
    )


def _build_widget_buttons(v):
    v.button("Default", key="dec-widget-default")
    v.button("Primary", primary=True, key="dec-widget-primary")
    v.button("Ghost", key="dec-widget-ghost").cls("dcs-btn dcs-btn--ghost")
    v.button("Danger", key="dec-widget-danger").cls("dcs-btn dcs-btn--danger")
    v.button_group(
        "Tool",
        ["Select", "Move", "Rotate", "Scale"],
        "Select",
        key="dec-widget-tool",
    )


def _build_widget_fields(v):
    v.input("Project", "untitled.dcs", key="dec-widget-project")
    v.textarea(
        "Notes",
        "safe to render - caches warm",
        key="dec-widget-notes",
    )
    v.input("Frequency", "440.0", type="number", key="dec-widget-frequency")
    v.input("Color", "#4d9fff", type="color", key="dec-widget-color")
    v.dropdown(
        "Renderer",
        ["Cycles - Pathtraced", "Eevee - Realtime", "Wireframe only"],
        "Cycles - Pathtraced",
        key="dec-widget-renderer",
    )


def _build_form_layout(v):
    def build_form(form):
        form.input("Project", "untitled.dcs", key="dec-form-project")
        form.textarea(
            "Notes",
            "safe to render - caches warm",
            key="dec-form-notes",
        )
        form.dropdown(
            "Renderer",
            ["Cycles - Pathtraced", "Eevee - Realtime", "Wireframe only"],
            "Cycles - Pathtraced",
            key="dec-form-renderer",
        )
        form.colorfield("Color", "#4d9fff", key="dec-form-color")
        form.button("Apply", primary=True, key="dec-form-apply")

    v.container(
        classes="dcs-form",
        key="dec-form-layout-widget",
        build=build_form,
    ).attr("style", "max-width:460px")


def _build_widget_ranges(v):
    v.slider("Roughness", 0.62, 0.0, 1.0, key="dec-widget-roughness")
    v.slider("Pan", -0.2, -1.0, 1.0, key="dec-widget-pan")
    v.knob("Cutoff", 0.65, 0.0, 1.0, key="dec-widget-cutoff")
    v.knob("Detune", 0.54, -1.0, 1.0, True, key="dec-widget-detune")
    v.input("Rotation", "45", type="number", key="dec-widget-rotation")


def _build_widget_selection(v):
    v.checkbox("Cast shadows", True, key="dec-widget-cast")
    v.checkbox("Receive shadows", False, key="dec-widget-receive")
    v.checkbox("Ray visible", True, key="dec-widget-ray")
    v.button_group(
        "Solver",
        ["BVH", "Embree", "kd-Tree"],
        "Embree",
        key="dec-widget-solver",
    )


def build_foundations(ctx, v):
    _note(ctx, v, "decius-foundations")
    _section(
        ctx,
        v,
        "Tokens, Badges, Icons",
        "dec-foundation-tokens",
        """
        <div class="dcs-col" style="gap:12px">
          <div class="dcs-row" style="gap:8px;flex-wrap:wrap">
            <span class="dcs-badge dcs-badge--accent">accent</span>
            <span class="dcs-badge dcs-badge--ok dcs-badge--dot">ok</span>
            <span class="dcs-badge dcs-badge--warn dcs-badge--dot">warn</span>
            <span class="dcs-badge dcs-badge--danger dcs-badge--dot">danger</span>
            <kbd class="dcs-kbd">Ctrl</kbd><kbd class="dcs-kbd">Shift</kbd><kbd class="dcs-kbd">K</kbd>
          </div>
          <div class="dcs-row" style="gap:12px;flex-wrap:wrap;font-size:20px">
            <i class="di di-decius"></i><i class="di di-cube"></i><i class="di di-layers"></i>
            <i class="di di-brush"></i><i class="di di-curve"></i><i class="di di-render"></i>
            <i class="di di-timeline"></i><i class="di di-cog"></i>
          </div>
          <div class="dcs-note">Token check: text, dim text, accent, semantic badges, and icon font.</div>
        </div>
        """,
    )


def build_buttons(ctx, v):
    _note(ctx, v, "decius-buttons")
    ctx.section(
        v,
        "Widget API Buttons",
        "props",
        "dec-widget-buttons",
        _build_widget_buttons,
    )
    _section(
        ctx,
        v,
        "Buttons",
        "dec-buttons",
        """
        <div class="dcs-col" style="gap:12px">
          <div class="dcs-row" style="gap:8px;flex-wrap:wrap">
            <button class="dcs-btn">Default</button>
            <button class="dcs-btn dcs-btn--primary"><i class="di di-play"></i> Primary</button>
            <button class="dcs-btn dcs-btn--ghost"><i class="di di-cog"></i> Ghost</button>
            <button class="dcs-btn dcs-btn--danger"><i class="di di-trash"></i> Danger</button>
            <button class="dcs-btn" disabled>Disabled</button>
            <button class="dcs-btn" aria-pressed="true">Pressed</button>
          </div>
          <div class="dcs-btn-group">
            <button class="dcs-btn" aria-pressed="true"><i class="di di-select"></i> Select</button>
            <button class="dcs-btn"><i class="di di-move"></i> Move</button>
            <button class="dcs-btn"><i class="di di-rotate"></i> Rotate</button>
            <button class="dcs-btn"><i class="di di-scale"></i> Scale</button>
          </div>
        </div>
        """,
    )
    _section(
        ctx,
        v,
        "Toolbars",
        "dec-toolbars",
        """
        <div class="dcs-col" style="gap:12px">
          <div class="dcs-toolbar">
            <span class="dcs-grip dcs-grip--h"></span>
            <button class="dcs-btn dcs-btn--icon dcs-btn--ghost" aria-pressed="true"><i class="di di-select"></i></button>
            <button class="dcs-btn dcs-btn--icon dcs-btn--ghost"><i class="di di-brush"></i></button>
            <button class="dcs-btn dcs-btn--icon dcs-btn--ghost"><i class="di di-eraser"></i></button>
            <span class="dcs-toolbar__sep"></span>
            <button class="dcs-btn dcs-btn--icon dcs-btn--ghost"><i class="di di-undo"></i></button>
            <button class="dcs-btn dcs-btn--icon dcs-btn--ghost"><i class="di di-redo"></i></button>
            <span class="dcs-toolbar__spacer"></span>
            <button class="dcs-btn dcs-btn--sm dcs-btn--primary">Apply</button>
          </div>
          <div style="position:relative;min-height:120px;background:radial-gradient(ellipse at 50% 40%,#3a4054,#161922 80%);overflow:hidden">
            <div class="dcs-toolbar dcs-toolbar--floating dcs-toolbar--sm" style="left:10px;top:10px" data-dcs-drag-bounds="sel">
              <span class="dcs-grip dcs-grip--h" data-dcs-drag-handle></span>
              <button class="dcs-btn dcs-btn--icon dcs-btn--sm dcs-btn--ghost" aria-pressed="true"><i class="di di-move"></i></button>
              <button class="dcs-btn dcs-btn--icon dcs-btn--sm dcs-btn--ghost"><i class="di di-lasso"></i></button>
              <button class="dcs-btn dcs-btn--icon dcs-btn--sm dcs-btn--ghost"><i class="di di-fill"></i></button>
            </div>
          </div>
        </div>
        """,
    )


def build_inputs(ctx, v):
    _note(ctx, v, "decius-inputs")
    ctx.section(
        v,
        "Widget API Fields",
        "props",
        "dec-widget-fields",
        _build_widget_fields,
    )
    ctx.section(
        v,
        "Form Layout",
        "split",
        "dec-form-layout",
        _build_form_layout,
    )
    _section(
        ctx,
        v,
        "Props Layout",
        "dec-props-layout",
        """
        <div class="dcs-props" style="max-width:520px">
          <div class="dcs-field"><label class="dcs-field__label">Name</label><input class="dcs-input" value="Cylinder.042"></div>
          <div class="dcs-field"><label class="dcs-field__label">Blend</label><div class="dcs-btn-group"><button class="dcs-btn" aria-pressed="true">Norm</button><button class="dcs-btn">Add</button><button class="dcs-btn">Mul</button></div></div>
          <div class="dcs-field"><label class="dcs-field__label">Quality</label><select class="dcs-select"><option>High</option><option>Medium</option><option>Draft</option></select></div>
          <div class="dcs-field"><label class="dcs-field__label">Live preview</label><div class="dcs-switch" aria-checked="true"></div></div>
          <div class="dcs-note">Props stretch controls across the channel column.</div>
        </div>
        """,
    )


def build_ranges(ctx, v):
    _note(ctx, v, "decius-ranges")
    ctx.section(
        v,
        "Widget API Ranges",
        "props",
        "dec-widget-ranges",
        _build_widget_ranges,
    )
    _section(
        ctx,
        v,
        "Sliders And Faders",
        "dec-sliders",
        """
        <div class="dcs-col" style="gap:18px;max-width:520px">
          <div class="dcs-field"><span class="dcs-field__label">Roughness</span><div data-dcs-slider data-min="0" data-max="1" data-value="0.62"></div><span class="dcs-mono">0.620</span></div>
          <div class="dcs-field"><span class="dcs-field__label">Pan</span><div data-dcs-slider data-min="-1" data-max="1" data-value="-0.2" data-bipolar></div><span class="dcs-mono">-0.20</span></div>
          <div class="dcs-row" style="gap:24px;align-items:flex-end;padding-top:8px">
            <div data-dcs-fader data-min="0" data-max="1" data-value="0.75" style="height:120px"></div>
            <div data-dcs-fader data-min="0" data-max="1" data-value="0.50" style="height:120px"></div>
            <div data-dcs-fader data-min="0" data-max="1" data-value="0.92" style="height:120px"></div>
          </div>
        </div>
        """,
    )
    _section(
        ctx,
        v,
        "Knobs And Combo Fields",
        "dec-knobs-combo",
        """
        <div class="dcs-col" style="gap:18px">
          <div class="dcs-row" style="gap:32px;flex-wrap:wrap">
            <div data-dcs-knob data-min="0" data-max="1" data-value="0.65" data-label="CUT"></div>
            <div data-dcs-knob data-min="0" data-max="1" data-value="0.30" data-label="RES"></div>
            <div data-dcs-knob data-min="-1" data-max="1" data-value="0.20" data-bipolar data-label="PAN"></div>
          </div>
          <div class="dcs-props" style="max-width:520px">
            <div class="dcs-field"><span class="dcs-field__label">Position</span><div data-dcs-combo data-label="X" data-min="-10" data-max="10" data-step="0.001" data-value="1.428"></div><div data-dcs-combo data-label="Y" data-min="-10" data-max="10" data-step="0.001" data-value="-0.952"></div><div data-dcs-combo data-label="Z" data-min="-10" data-max="10" data-step="0.001" data-value="3"></div></div>
            <div class="dcs-field"><span class="dcs-field__label">Rotation</span><div data-dcs-combo data-min="-180" data-max="180" data-step="1" data-value="45" data-suffix=" deg"></div></div>
          </div>
        </div>
        """,
    )


def build_checks(ctx, v):
    _note(ctx, v, "decius-checks")
    ctx.section(
        v,
        "Widget API Selection",
        "props",
        "dec-widget-selection",
        _build_widget_selection,
    )
    _section(
        ctx,
        v,
        "Checks, Radios, Switches",
        "dec-checks",
        """
        <div class="dcs-row" style="gap:34px;align-items:flex-start;flex-wrap:wrap">
          <div class="dcs-col">
            <label class="dcs-check" aria-checked="true"><span class="dcs-check__box"></span><span>Cast shadows</span></label>
            <label class="dcs-check"><span class="dcs-check__box"></span><span>Receive shadows</span></label>
            <label class="dcs-check" aria-checked="true"><span class="dcs-check__box"></span><span>Ray visible</span></label>
          </div>
          <div class="dcs-col">
            <label class="dcs-radio" data-dcs-name="solver"><span class="dcs-check__box"></span><span>BVH</span></label>
            <label class="dcs-radio" data-dcs-name="solver" aria-checked="true"><span class="dcs-check__box"></span><span>Embree</span></label>
            <label class="dcs-radio" data-dcs-name="solver"><span class="dcs-check__box"></span><span>kd-Tree</span></label>
          </div>
          <div class="dcs-col">
            <div class="dcs-row"><span class="dcs-switch" aria-checked="true"></span><span>Auto-bake</span></div>
            <div class="dcs-row"><span class="dcs-switch"></span><span>Cache to disk</span></div>
            <div class="dcs-row"><span class="dcs-switch" aria-checked="true"></span><span>Watch live</span></div>
          </div>
        </div>
        """,
    )


def build_layout(ctx, v):
    _note(ctx, v, "decius-layout")
    _section(
        ctx,
        v,
        "Panels And Tabs",
        "dec-panels",
        """
        <div class="dcs-col" style="gap:12px">
          <div class="dcs-panel">
            <div class="dcs-panel__header"><div class="dcs-panel__title"><i class="di di-cog"></i><span>Properties</span></div><button class="dcs-btn dcs-btn--icon dcs-btn--sm dcs-btn--ghost"><i class="di di-more-h"></i></button></div>
            <div class="dcs-panel__body"><div class="dcs-props"><div class="dcs-field"><span class="dcs-field__label">Name</span><input class="dcs-input" value="Cube.003"></div><div class="dcs-field"><span class="dcs-field__label">Mass</span><div data-dcs-combo data-value="1" data-step="0.001"></div></div></div></div>
          </div>
          <div class="dcs-tabs">
            <button class="dcs-tab" aria-selected="true" data-dcs-target="#dec-tab-mesh"><i class="di di-cube"></i> Mesh</button>
            <button class="dcs-tab" data-dcs-target="#dec-tab-shading"><i class="di di-droplet"></i> Shading</button>
          </div>
          <div id="dec-tab-mesh" data-dcs-tabpanel>Mesh inspector content.</div>
          <div id="dec-tab-shading" data-dcs-tabpanel hidden>Shading inspector content.</div>
        </div>
        """,
    )
    _section(
        ctx,
        v,
        "Subpanels And Foldouts",
        "dec-foldouts",
        """
        <div class="dcs-foldouts" style="max-width:380px">
          <div class="dcs-foldout">
            <div class="dcs-foldout__header"><span class="dcs-foldout__chevron"><i class="di di-chevron-right"></i></span><i class="di di-move dcs-foldout__icon"></i><span class="dcs-foldout__title">Transform</span><span class="dcs-foldout__meta">Local</span></div>
            <div class="dcs-foldout__body"><div class="dcs-props"><div class="dcs-field"><span class="dcs-field__label">X</span><div data-dcs-combo data-value="1.428" data-step="0.001"></div></div><div class="dcs-field"><span class="dcs-field__label">Y</span><div data-dcs-combo data-value="-0.952" data-step="0.001"></div></div></div></div>
          </div>
          <div class="dcs-foldout dcs-foldout--collapsed">
            <div class="dcs-foldout__header"><span class="dcs-foldout__chevron"><i class="di di-chevron-right"></i></span><i class="di di-palette dcs-foldout__icon"></i><span class="dcs-foldout__title">Material</span><span class="dcs-foldout__meta">Lambert</span></div>
            <div class="dcs-foldout__body"><div class="dcs-field"><span class="dcs-field__label">Rough</span><div data-dcs-slider data-value="0.42"></div></div></div>
          </div>
        </div>
        """,
    )


def build_docking(ctx, v):
    _note(ctx, v, "decius-dock")
    _section(
        ctx,
        v,
        "Dock Panes And Splitters",
        "dec-dockpanes",
        """
        <div class="dcs-dock" style="height:360px;border:1px solid var(--dcs-line);background:var(--dcs-line)">
          <div class="dcs-dockpane" style="flex:0 0 220px">
            <div class="dcs-dockpane__tabbar"><div class="dcs-dockpane__tabs"><button class="dcs-dockpane__tab" aria-selected="true" data-dcs-target="#dec-dock-tree"><i class="di di-layers"></i> Tree</button><button class="dcs-dockpane__tab" data-dcs-target="#dec-dock-files"><i class="di di-folder"></i> Files</button></div></div>
            <div class="dcs-dockpane__body"><div id="dec-dock-tree" data-dcs-tabpanel><div class="dcs-tree" data-dcs-select><div class="dcs-tree__row" aria-selected="true" style="--depth:0"><span class="dcs-tree__label">Scene</span></div><div class="dcs-tree__row" style="--depth:1"><span class="dcs-tree__label">Camera</span></div></div></div><div id="dec-dock-files" data-dcs-tabpanel hidden><div class="dcs-list"><div class="dcs-list__item">intro.scene</div><div class="dcs-list__item">hero.mat</div></div></div></div>
          </div>
          <div class="dcs-splitter" data-dcs-splitter></div>
          <div class="dcs-dockpane dcs-dockpane--center" style="flex:1">
            <div class="dcs-dockpane__tabbar"><div class="dcs-dockpane__tabs"><button class="dcs-dockpane__tab" aria-selected="true" data-dcs-target="#dec-dock-doc"><i class="di di-image"></i> hero.psd</button></div></div>
            <div class="dcs-dockpane__body" style="position:relative"><div id="dec-dock-doc" data-dcs-tabpanel style="position:absolute;inset:0;background:radial-gradient(ellipse at 50% 40%,#3a4054,#161922 80%)"><div class="dcs-toolbar dcs-toolbar--floating dcs-toolbar--sm" style="left:12px;top:12px" data-dcs-drag-bounds="sel"><span class="dcs-grip dcs-grip--h" data-dcs-drag-handle></span><button class="dcs-btn dcs-btn--icon dcs-btn--sm dcs-btn--ghost"><i class="di di-move"></i></button><button class="dcs-btn dcs-btn--icon dcs-btn--sm dcs-btn--ghost"><i class="di di-brush"></i></button></div></div></div>
          </div>
        </div>
        """,
    )


def build_data_views(ctx, v):
    _note(ctx, v, "decius-data")
    _section(
        ctx,
        v,
        "Lists, Trees, Tables",
        "dec-data",
        """
        <div class="dcs-col" style="gap:16px">
          <div class="dcs-list" data-dcs-select="multi" style="max-width:380px">
            <div class="dcs-list__item" aria-selected="true"><i class="di di-palette"></i><span style="flex:1">Lambert.001</span><span class="dcs-tree__meta">#4d9fff</span></div>
            <div class="dcs-list__item"><i class="di di-image"></i><span style="flex:1">StudioBack.exr</span><span class="dcs-tree__meta">4k</span></div>
            <div class="dcs-list__item"><i class="di di-curve"></i><span style="flex:1">walk.anim</span><span class="dcs-tree__meta">240f</span></div>
          </div>
          <table class="dcs-table dcs-table--mono">
            <thead><tr><th></th><th>Job</th><th>Frames</th><th>Status</th></tr></thead>
            <tbody><tr aria-selected="true"><td><i class="di di-check-circle"></i></td><td>Scene_Intro_v014</td><td>1-240</td><td><span class="dcs-badge dcs-badge--ok dcs-badge--dot">Done</span></td></tr><tr><td><i class="di di-play"></i></td><td>Hero_Closeup_v003</td><td>48-96</td><td><span class="dcs-badge dcs-badge--accent dcs-badge--dot">62%</span></td></tr></tbody>
          </table>
        </div>
        """,
    )


def build_overlays(ctx, v):
    _note(ctx, v, "decius-overlays")
    _section(
        ctx,
        v,
        "Menus And Popovers",
        "dec-menus",
        """
        <div class="dcs-col" style="gap:14px">
          <div class="dcs-menubar">
            <div class="dcs-menubar__brand"><i class="di di-decius"></i><span>workspace</span></div>
            <button class="dcs-menubar__item" data-dcs-toggle="menu" data-dcs-target="#dec-menu-file">File</button>
            <button class="dcs-menubar__item" data-dcs-toggle="menu" data-dcs-target="#dec-menu-view">View</button>
            <span class="dcs-menubar__spacer"></span>
            <span class="dcs-menubar__meta">untitled.scene</span>
          </div>
          <div class="dcs-row" style="gap:12px">
            <button class="dcs-btn dcs-btn--primary" data-dcs-toggle="menu" data-dcs-target="#dec-menu-options"><i class="di di-menu"></i> Dropdown</button>
            <button class="dcs-btn" data-dcs-toggle="popover" data-dcs-target="#dec-popover-info" data-dcs-placement="bottom"><i class="di di-info"></i> Popover</button>
            <div class="dcs-well" data-dcs-menu="#dec-menu-context" style="padding:14px;border-radius:5px;color:var(--dcs-text-mute)">right-click context menu</div>
          </div>
          <div class="dcs-menu" id="dec-menu-file" hidden><div class="dcs-menu__label">File</div><div class="dcs-menu__item" data-dcs-value="new"><span class="dcs-menu__icon"><i class="di di-file"></i></span><span class="dcs-menu__label-text">New</span><span class="dcs-menu__shortcut">Ctrl N</span></div><div class="dcs-menu__item" data-dcs-value="open"><span class="dcs-menu__icon"><i class="di di-folder-open"></i></span><span class="dcs-menu__label-text">Open</span></div></div>
          <div class="dcs-menu" id="dec-menu-view" hidden><div class="dcs-menu__item"><span class="dcs-menu__check"><i class="di di-check"></i></span><span class="dcs-menu__label-text">Grid</span></div><div class="dcs-menu__item"><span class="dcs-menu__label-text">Fullscreen</span></div></div>
          <div class="dcs-menu" id="dec-menu-options" hidden><div class="dcs-menu__item"><span class="dcs-menu__label-text">Wireframe</span></div><div class="dcs-menu__item"><span class="dcs-menu__label-text">Solid</span></div><div class="dcs-menu__item dcs-menu__item--has-sub"><span class="dcs-menu__label-text">Export As</span><span class="dcs-menu__caret"><i class="di di-chevron-right"></i></span><div class="dcs-menu dcs-menu__sub"><div class="dcs-menu__item">PNG</div><div class="dcs-menu__item">EXR</div></div></div></div>
          <div class="dcs-menu" id="dec-menu-context" hidden><div class="dcs-menu__item"><span class="dcs-menu__icon"><i class="di di-copy"></i></span><span class="dcs-menu__label-text">Copy</span></div><div class="dcs-menu__item dcs-menu__item--danger"><span class="dcs-menu__icon"><i class="di di-trash"></i></span><span class="dcs-menu__label-text">Delete</span></div></div>
          <div class="dcs-popover" id="dec-popover-info" style="width:240px" hidden><div class="dcs-popover__header">Layer settings<span style="flex:1"></span><span class="dcs-toast__close" data-dcs-dismiss="popover"><i class="di di-close"></i></span></div><div class="dcs-popover__body"><div class="dcs-field"><span class="dcs-field__label">Opacity</span><div data-dcs-slider data-value="0.8"></div></div></div><div class="dcs-popover__arrow"></div></div>
        </div>
        """,
    )
    _section(
        ctx,
        v,
        "Toasts",
        "dec-toasts",
        """
        <div class="dcs-toasts" style="position:static;width:320px">
          <div class="dcs-toast dcs-toast--ok"><div class="dcs-toast__icon"><i class="di di-check-circle"></i></div><div class="dcs-toast__body"><div class="dcs-toast__title">Saved</div><div class="dcs-toast__msg">scene.blend written</div></div><div class="dcs-toast__close"><i class="di di-close"></i></div></div>
          <div class="dcs-toast dcs-toast--warn"><div class="dcs-toast__icon"><i class="di di-alert"></i></div><div class="dcs-toast__body"><div class="dcs-toast__title">Heads up</div><div class="dcs-toast__msg">Unsaved changes</div></div></div>
        </div>
        """,
    )


def build_feedback(ctx, v):
    _note(ctx, v, "decius-feedback")
    _section(
        ctx,
        v,
        "Alerts, Modal, Status",
        "dec-feedback",
        """
        <div class="dcs-col" style="gap:12px">
          <div class="dcs-alert dcs-alert--ok"><div class="dcs-alert__icon"><i class="di di-check-circle"></i></div><div class="dcs-alert__body"><div class="dcs-alert__title">Bake complete</div><div class="dcs-alert__msg">240 frames cached to ./cache/intro_v014</div></div><button class="dcs-btn dcs-btn--icon dcs-btn--sm dcs-btn--ghost"><i class="di di-close"></i></button></div>
          <div class="dcs-alert dcs-alert--warn"><div class="dcs-alert__icon"><i class="di di-alert"></i></div><div class="dcs-alert__body"><div class="dcs-alert__title">GPU clipping detected</div><div class="dcs-alert__msg">2 channels exceeded headroom on frame 081.</div></div></div>
          <div class="dcs-modal-backdrop" style="position:relative;display:flex;padding:18px;background:rgba(0,0,0,.28)">
            <div class="dcs-modal" style="width:420px;position:relative"><div class="dcs-modal__header"><i class="di di-save"></i><span>Save project as</span></div><div class="dcs-modal__body"><div class="dcs-field"><label class="dcs-field__label">Name</label><input class="dcs-input" value="Scene_Intro_v014.dcs"></div></div><div class="dcs-modal__footer"><button class="dcs-btn">Cancel</button><button class="dcs-btn dcs-btn--primary">Save</button></div></div>
          </div>
          <div class="dcs-statusbar"><span class="dcs-statusbar__item dcs-statusbar__item--ok"><i class="di di-check-circle"></i> Ready</span><span class="dcs-statusbar__spacer"></span><span class="dcs-statusbar__item dcs-statusbar__item--accent">native DOM script attached</span></div>
        </div>
        """,
    )


def build_editors(ctx, v):
    _note(ctx, v, "decius-editors")
    _section(
        ctx,
        v,
        "Color, Curve, Graph",
        "dec-editors",
        """
        <div class="dcs-col" style="gap:16px">
          <div class="dcs-row" style="gap:12px;align-items:flex-start;flex-wrap:wrap">
            <div class="dcs-color-square" style="width:160px;height:120px;background:linear-gradient(90deg,#fff,rgba(255,255,255,0)),linear-gradient(0deg,#000,#4d9fff)"></div>
            <div class="dcs-hue-bar" style="width:18px;height:120px;background:linear-gradient(180deg,red,yellow,lime,cyan,blue,magenta,red)"></div>
            <div class="dcs-col" style="gap:8px"><span class="dcs-swatch"><span class="dcs-swatch__chip" style="--c:#4d9fff;background:#4d9fff"></span><span>#4D9FFF</span></span><input class="dcs-input" value="#4D9FFF"></div>
          </div>
          <div class="dcs-graph" style="height:160px;position:relative;background:linear-gradient(var(--dcs-line-soft) 1px,transparent 1px),linear-gradient(90deg,var(--dcs-line-soft) 1px,transparent 1px);background-size:32px 32px"><svg viewBox="0 0 360 140" style="position:absolute;inset:10px;width:calc(100% - 20px);height:calc(100% - 20px)"><path d="M10,110 C90,40 160,120 230,52 S330,22 350,70" fill="none" stroke="var(--dcs-accent)" stroke-width="2"/><circle cx="90" cy="40" r="4" fill="var(--dcs-accent)"/><circle cx="230" cy="52" r="4" fill="var(--dcs-accent)"/></svg></div>
        </div>
        """,
    )


def build_hardware(ctx, v):
    _note(ctx, v, "decius-hardware")
    _section(
        ctx,
        v,
        "Skeuomorphic Hardware",
        "dec-hardware",
        """
        <div class="dcs-hw dcs-hw--red" style="position:relative;max-width:540px;padding:18px">
          <span class="dcs-hw__screw dcs-hw__screw--tl"></span><span class="dcs-hw__screw dcs-hw__screw--tr"></span>
          <fieldset class="dcs-silk"><legend>FILTER</legend>
            <div class="dcs-row" style="gap:26px;justify-content:center">
              <div data-dcs-knob data-value="0.65" data-label="CUT"></div>
              <div data-dcs-knob data-value="0.30" data-label="RES"></div>
              <div data-dcs-knob data-value="0.45" data-label="DRIVE"></div>
            </div>
          </fieldset>
          <div class="dcs-row" style="gap:12px;margin-top:14px;align-items:center">
            <span class="dcs-hw__label">PATCH</span>
            <button class="dcs-btn dcs-btn--sm dcs-btn--primary">hot</button>
            <div data-dcs-slider data-value="0.58" style="flex:1"></div>
          </div>
        </div>
        """,
    )
