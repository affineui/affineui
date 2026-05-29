"""Overview panel for the AffineUI Python test bench."""


def build(ctx, v):
    """Build the overview page."""

    v.paragraph(
        "A native and browser-comparable test surface for exercising framework "
        "selectors, command widgets, interaction, layout, and retained handles.",
        classes=ctx.note_class(),
        key="overview-lede",
    )

    def build_cards(card_view):
        _card(
            ctx,
            card_view,
            "Controls",
            "Buttons, toggles, checkboxes, sliders, knobs, dropdowns, and color chips.",
            "overview-card-controls",
        )
        _card(
            ctx,
            card_view,
            "Forms",
            "Plain form flow, inspector-style property rows, text editors, and value editors.",
            "overview-card-forms",
        )
        _card(
            ctx,
            card_view,
            "Lists & Tree",
            "Scrollable list rows, tree indentation, disclosure affordances, and selection spans.",
            "overview-card-collections",
        )
        _card(
            ctx,
            card_view,
            "Responsive Shell",
            "Resize the window to verify the side navigation collapses into the mobile selector.",
            "overview-card-responsive",
        )

    v.container(classes="aui-demo-grid", key="overview-cards", build=build_cards)


def _card(ctx, v, title, body, key):
    def build_card(card):
        card.paragraph(body, key=f"{key}-body")

    ctx.section(v, title, "props", key, build_card)
