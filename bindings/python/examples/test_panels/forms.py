"""Form and property editor panels for the AffineUI Python test bench."""


def build(ctx, v):
    """Build form and inspector layout examples."""

    v.paragraph(
        "The same command widgets are shown in framework-provided form and "
        "property layouts so spacing, prompts, and stretch behavior are easy to compare.",
        classes=ctx.note_class(),
        key="forms-note",
    )

    v.button_group(
        "Layout",
        ["Split", "Form", "Props"],
        ctx.field_layout.title(),
        key="field-layout",
    ).on_change(lambda value: ctx.set_field_layout(value.lower()))

    def build_stack(stack):
        mode = ctx.field_layout
        ctx.section(
            stack,
            {
                "split": "Plain Split Flow",
                "form": "Centered Form Flow",
                "props": "Inspector Properties",
            }[mode],
            mode,
            f"fields-{mode}",
            lambda scoped, current_mode=mode: _field_section(ctx, scoped, current_mode),
        )

    v.container(classes="aui-demo-stack", key="field-layout-stack", build=build_stack)


def _field_section(ctx, v, mode):
    v.input("Object name", value="Cylinder 042", key=f"{mode}-text").on_change(
        lambda value, current_mode=mode: print(f"{current_mode} name changed: {value}")
    )
    v.input(
        "Password",
        value="secret",
        type="password",
        key=f"{mode}-password",
    ).on_change(lambda value, current_mode=mode: print(f"{current_mode} password changed: {value}"))
    v.input(
        "Gain",
        value="1.000",
        type="number",
        key=f"{mode}-number",
    ).on_change(lambda value, current_mode=mode: print(f"{current_mode} gain changed: {value}"))
    v.input(
        "Accent",
        value="#33aaff",
        type="color",
        key=f"{mode}-color",
    ).on_change(lambda value, current_mode=mode: print(f"{current_mode} accent changed: {value}"))
    v.dropdown(
        "Object type",
        ["Mesh", "Light", "Camera", "Audio"],
        "Mesh",
        key=f"{mode}-dropdown",
    ).on_change(lambda value, current_mode=mode: print(f"{current_mode} type changed: {value}"))
    v.textarea(
        "Notes",
        "Dense native UI, browser semantics, retained handles.",
        key=f"{mode}-textarea",
    ).on_change(lambda value, current_mode=mode: print(f"{current_mode} notes changed: {value}"))
    v.button("Apply", primary=(mode == ctx.field_layout), key=f"{mode}-apply").on_click(
        lambda current_mode=mode: print(f"{current_mode} apply clicked")
    )
