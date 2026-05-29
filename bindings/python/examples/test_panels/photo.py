"""Hybrid Decius photo-editor panel driven by Python on the C++ runtime."""


def build(ctx, v):
    v.paragraph(
        "Python owns this editor shell while C++ supplies the retained view tree, "
        "DOM scripting, layout, and widget callbacks.",
        classes=ctx.note_class(),
        key="photo-note",
    )

    def build_shell(shell):
        def build_tools(tools):
            for label, tool in (
                ("Move", "move"),
                ("Brush", "brush"),
                ("Lasso", "lasso"),
                ("Crop", "crop"),
                ("Text", "text"),
            ):
                active = ctx.photo_tool == tool
                button = tools.button(label, primary=active, key=f"photo-tool-{tool}")
                if ctx.is_decius():
                    button.cls("dcs-btn dcs-btn--sm" + (" dcs-btn--primary" if active else ""))
                button.attr("aria-pressed", "true" if active else "false")
                button.on_click(lambda next_tool=tool: ctx.set_photo_tool(next_tool))

        shell.container(
            classes="photo-hybrid-toolbar",
            key="photo-hybrid-toolbar",
            build=build_tools,
        )

        def build_body(body):
            body.container(
                classes="photo-hybrid-tools",
                key="photo-hybrid-tools",
                build=_build_tool_column(ctx),
            )
            body.container(
                classes="photo-hybrid-canvas",
                key="photo-hybrid-canvas",
                build=_build_canvas(ctx),
            )
            body.container(
                classes="photo-hybrid-inspector",
                key="photo-hybrid-inspector",
                build=_build_inspector(ctx),
            )

        shell.container(
            classes="photo-hybrid-body",
            key="photo-hybrid-body",
            build=build_body,
        )

    v.container(classes="photo-hybrid-shell", key="photo-hybrid", build=build_shell)


def _build_tool_column(ctx):
    def build(v):
        for label, tool in (
            ("M", "move"),
            ("B", "brush"),
            ("L", "lasso"),
            ("C", "crop"),
            ("T", "text"),
        ):
            active = ctx.photo_tool == tool
            button = v.button(label, primary=active, key=f"photo-rail-{tool}")
            if ctx.is_decius():
                button.cls("dcs-btn dcs-btn--icon" + (" dcs-btn--primary" if active else ""))
            button.attr("title", tool.title())
            button.attr("aria-pressed", "true" if active else "false")
            button.on_click(lambda next_tool=tool: ctx.set_photo_tool(next_tool))

    return build


def _build_canvas(ctx):
    def build(v):
        v.container(classes="photo-hybrid-ruler photo-hybrid-ruler--top", key="photo-ruler-top")
        v.container(classes="photo-hybrid-ruler photo-hybrid-ruler--left", key="photo-ruler-left")
        v.container(classes="photo-hybrid-artboard", key="photo-artboard", build=_build_artboard(ctx))
        v.container(classes="photo-hybrid-status", key="photo-status", build=_build_status(ctx))

    return build


def _build_artboard(ctx):
    def build(v):
        v.container(classes="photo-hybrid-photo", key="photo-bitmap")
        v.container(classes="photo-hybrid-selection", key="photo-selection")
        v.paragraph(ctx.photo_tool.title(), classes="photo-hybrid-tool-label", key="photo-tool-label")

    return build


def _build_status(ctx):
    def build(v):
        v.paragraph("67%", key="photo-zoom")
        v.paragraph("RGB/8", key="photo-mode")
        v.paragraph(f"Tool: {ctx.photo_tool.title()}", key="photo-tool-status")

    return build


def _build_inspector(ctx):
    def build(v):
        v.heading(2, "Layers", key="photo-layers-title")
        for label, layer in (
            ("Adjustment - Curves", "layer-curves"),
            ("Hero retouch", "layer-hero"),
            ("Background plate", "layer-bg"),
        ):
            selected = ctx.photo_layer == layer
            row = v.button(label, primary=selected, key=layer)
            if ctx.is_decius():
                row.cls("dcs-list__item").attr(
                    "aria-selected", "true" if selected else "false"
                )
            row.on_click(lambda next_layer=layer: ctx.set_photo_layer(next_layer))

        v.heading(2, "Properties", key="photo-props-title")
        v.slider("Opacity", 0.82, 0.0, 1.0, key="photo-opacity")
        v.dropdown("Blend", ["Normal", "Multiply", "Screen", "Overlay"], "Normal", key="photo-blend")
        v.input("Tint", "#3bb7ff", type="color", key="photo-tint")
        v.checkbox("Show transform controls", True, key="photo-transform")

    return build
