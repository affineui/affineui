"""List and tree panels for the AffineUI Python test bench.

Both panels are backed by the recycling virtual list/tree: only the rows under
the viewport are ever built, so the 50,000-row list and the scene tree scroll
smoothly and keep selection while the surrounding panel is rebuilt. Selection
supports plain / Ctrl (toggle) / Shift (range) clicks; tree branches expand and
collapse via their chevrons.
"""


def build(ctx, v):
    """Build the virtual list and virtual tree examples."""

    v.paragraph(
        "50,000 rows and a scene tree, both virtualized: only the visible rows "
        "render, the scrollbar spans the full extent, and selection (plain / "
        "Ctrl / Shift) survives scrolling and rebuilds.",
        classes=ctx.note_class(),
        key="collections-note",
    )

    # Checkbox MODE: rows carry two independent states — selected and
    # checked. The toggle applies to both the list and the tree.
    v.checkbox(
        "Row checkboxes (list + tree)",
        ctx.collections_checkboxes,
        key="collections-cb-mode",
    ).on_click(ctx.toggle_collections_checkboxes)

    # The virtual list/tree element is itself the scroll box (the scroll ->
    # re-window seam listens on it), so the host must NOT scroll: it just gives
    # the widget a definite height (aui-virtual-host: 340px flex column).
    def build_list(section):
        section.container(
            classes="aui-virtual-host",
            key="list-scroll",
            build=ctx.build_list_rows,
        )

    def build_tree(section):
        section.container(
            classes="aui-virtual-host",
            key="tree-scroll",
            build=ctx.build_tree_rows,
        )

    ctx.section(v, "Virtual List (50k rows)", "props", "collections-list", build_list)
    ctx.section(v, "Virtual Tree", "props", "collections-tree", build_tree)
