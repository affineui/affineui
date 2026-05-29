"""Core interactive controls for the AffineUI Python test bench."""


def build(ctx, v):
    """Build buttons, selection controls, sliders, knobs, dropdowns, and color widgets."""

    v.paragraph(
        "Basic controls should render with the active framework personality and "
        "work without app-specific glue code.",
        classes=ctx.note_class(),
        key="controls-note",
    )

    def build_basic(section):
        section.button("Primary action", primary=True, key="controls-primary").on_click(
            lambda: print("Primary action clicked")
        )
        section.button("Secondary action", key="controls-secondary").on_click(
            lambda: print("Secondary action clicked")
        )
        section.checkbox("Enable preview", checked=True, key="controls-check").on_change(
            lambda value: print(f"preview changed: {value}")
        )
        section.button_group(
            "Mode",
            ["Object", "Edit", "Sculpt"],
            "Object",
            key="controls-mode",
        ).on_change(lambda value: print(f"mode changed: {value}"))

    def build_ranges(section):
        section.slider(
            "Opacity",
            value=0.65,
            min=0.0,
            max=1.0,
            key="controls-slider",
        ).on_change(lambda value: print(f"slider changed: {value}"))
        section.knob(
            "Cutoff",
            value=0.42,
            min=0.0,
            max=1.0,
            key="controls-knob",
        ).on_change(lambda value: print(f"knob changed: {value}"))
        section.input(
            "Tint",
            value="#3bb7ff",
            type="color",
            key="controls-color",
        ).on_change(lambda value: print(f"color changed: {value}"))

    def build_choices(section):
        section.dropdown(
            "Preset",
            ["Warm pad", "Digital pluck", "Sub bass", "Noise sweep"],
            "Warm pad",
            key="controls-preset",
        ).on_change(lambda value: print(f"preset changed: {value}"))
        section.checkbox("Loop selection", checked=False, key="controls-loop").on_change(
            lambda value: print(f"loop changed: {value}")
        )

    ctx.section(v, "Buttons And Toggles", "props", "controls-basic", build_basic)
    ctx.section(v, "Ranges", "props", "controls-ranges", build_ranges)
    ctx.section(v, "Choices", "props", "controls-choices", build_choices)
