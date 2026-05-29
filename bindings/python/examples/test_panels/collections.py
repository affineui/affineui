"""List and tree panels for the AffineUI Python test bench."""


def build(ctx, v):
    """Build scrollable list and tree examples."""

    v.paragraph(
        "Rows should fill the available width, live inside scroll containers, and retain "
        "selection while the surrounding panel is rebuilt.",
        classes=ctx.note_class(),
        key="collections-note",
    )

    def build_list(section):
        section.button("Append Row", key="list-append").on_click(ctx.append_row)
        section.container(classes="aui-scroll-list", key="list-scroll", build=ctx.build_list_rows)

    def build_tree(section):
        section.container(classes="aui-scroll-tree", key="tree-scroll", build=ctx.build_tree_rows)

    ctx.section(v, "Virtual List Target", "props", "collections-list", build_list)
    ctx.section(v, "Tree Target", "props", "collections-tree", build_tree)
