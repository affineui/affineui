import argparse
from typing import Optional

import affineui as ui


def parse_style(name: str) -> ui.ViewTheme:
    normalized = name.strip().lower()
    if normalized == "bootstrap":
        return ui.ViewTheme.Bootstrap
    if normalized == "decius":
        return ui.ViewTheme.Decius
    raise ValueError(f"unknown theme: {name}")


class HelloPanel:
    def __init__(self, theme: ui.ViewTheme) -> None:
        self.theme = theme
        self.app: Optional[ui.App] = None
        self.active_tab = "controls"
        self.field_layout = "props"
        self.rows = [f"Frame {index:02d} - event routed" for index in range(1, 33)]
        self.selected_row = 0
        self.selected_tree = "tree-camera"

    def _is_decius(self) -> bool:
        return self.theme == ui.ViewTheme.Decius

    def _tab_button(self, v: ui.View, label: str, tab: str) -> None:
        active = self.active_tab == tab
        button = v.button(label, primary=active, key=f"tab-{tab}")
        button.on_click(lambda tab=tab: self.select_tab(tab))
        if self._is_decius():
            button.cls("dcs-tab").attr("aria-selected", "true" if active else "false")
        else:
            button.cls("nav-link active" if active else "nav-link")

    def _build_tabs(self, v: ui.View) -> None:
        self._tab_button(v, "Controls", "controls")
        self._tab_button(v, "Fields", "fields")
        self._tab_button(v, "Lists & tree", "collections")

    def _flow_class(self, mode: str) -> str:
        if self._is_decius():
            return (
                ui.decius.class_name.form
                if mode == "form"
                else ui.decius.class_name.props
            )
        return (
            ui.bootstrap.class_name.form
            if mode == "form"
            else ui.bootstrap.class_name.props
        )

    def _build_controls(self, v: ui.View) -> None:
        v.button("Hello button", primary=True, key="hello-button").on_click(
            lambda: print("Hello button clicked")
        )
        v.checkbox("Framework checkbox", checked=True, key="hello-check").on_change(
            lambda value: print(f"checkbox changed: {value}")
        )
        v.slider(
            "Framework slider",
            value=0.65,
            min=0.0,
            max=1.0,
            key="hello-slider",
        ).on_change(lambda value: print(f"slider changed: {value}"))
        v.knob(
            "Framework knob",
            value=0.42,
            min=0.0,
            max=1.0,
            key="hello-knob",
        ).on_change(lambda value: print(f"knob changed: {value}"))

    def _build_fields(self, v: ui.View) -> None:
        v.button_group(
            "Field layout",
            ["Form", "Props"],
            "Form" if self.field_layout == "form" else "Props",
            key="field-layout",
        ).on_change(lambda value: self.set_field_layout(value.lower()))
        v.input("Object name", "Cylinder.042", key="object-name").on_change(
            lambda value: print(f"name changed: {value}")
        )
        v.password("Token", "secret", key="token").on_change(
            lambda value: print(f"token changed: {value}")
        )
        v.input("Gain", "1.000", type="number", key="gain-field").on_change(
            lambda value: print(f"gain changed: {value}")
        )
        v.input("Tint", "#4da3ff", type="color", key="tint-color").on_change(
            lambda value: print(f"tint changed: {value}")
        )
        v.slider(
            "Rotation",
            value=45.0,
            min=-180.0,
            max=180.0,
            key="rotation-degrees",
        ).on_change(lambda value: print(f"rotation changed: {value}"))
        v.dropdown(
            "Mode",
            ["Object", "Edit", "Sculpt", "Render"],
            "Object",
            key="mode",
        ).on_change(lambda value: print(f"mode changed: {value}"))
        v.button_group(
            "Transform space",
            ["Local", "World", "View"],
            "World",
            key="space",
        ).on_change(lambda value: print(f"space changed: {value}"))
        v.textarea(
            "Notes",
            "Dense native UI, browser semantics.",
            rows=3,
            key="notes",
        ).on_change(lambda value: print(f"notes changed: {value}"))

    def _select_row(self, index: int) -> None:
        self.selected_row = index
        self.reload()

    def _append_row(self) -> None:
        self.rows.append(f"Generated row {len(self.rows) + 1:02d}")
        self.selected_row = len(self.rows) - 1
        self.reload()

    def _build_row(self, v: ui.View, index: int) -> None:
        title = self.rows[index]
        selected = index == self.selected_row
        row = v.button(title, primary=selected, key=f"log-row-{index}")
        row.on_click(lambda index=index: self._select_row(index))
        if self._is_decius():
            row.cls(ui.decius.class_name.list_item).attr(
                "aria-selected", "true" if selected else "false"
            )
        else:
            row.cls(
                f"{ui.bootstrap.class_name.list_item} list-group-item-action"
                + (" active" if selected else "")
            )

    def _select_tree(self, key: str) -> None:
        self.selected_tree = key
        self.reload()

    def _tree_row(self, v: ui.View, label: str, key: str, depth: int = 0) -> None:
        selected = self.selected_tree == key
        row = v.button(label, primary=selected, key=key)
        row.on_click(lambda key=key: self._select_tree(key))
        indent = 12 + depth * 18
        if self._is_decius():
            row.cls(ui.decius.class_name.tree_row).attr(
                "aria-selected", "true" if selected else "false"
            )
        else:
            row.cls(
                f"{ui.bootstrap.class_name.tree_row} list-group-item-action"
                + (" active" if selected else "")
            )
        row.attr("style", f"padding-left:{indent}px")

    def _build_tree_rows(self, v: ui.View) -> None:
        self._tree_row(v, "Scene", "tree-scene", 0)
        self._tree_row(v, "Camera", "tree-camera", 1)
        self._tree_row(v, "Key Light", "tree-light", 1)
        self._tree_row(v, "Player", "tree-player", 1)
        self._tree_row(v, "Mesh", "tree-player-mesh", 2)
        self._tree_row(v, "Controller", "tree-player-controller", 2)
        self._tree_row(v, "Environment", "tree-environment", 1)
        self._tree_row(v, "Collision", "tree-collision", 2)
        self._tree_row(v, "Audio", "tree-audio", 1)

    def _build_collections(self, v: ui.View) -> None:
        note_class = (
            ui.decius.class_name.note
            if self._is_decius()
            else ui.bootstrap.class_name.note
        )
        v.paragraph("Virtual event log", classes=note_class, key="list-prompt")
        v.button("Append row", primary=True, key="append-row").on_click(self._append_row)
        list_classes = (
            ui.decius.class_name.list
            if self._is_decius()
            else ui.bootstrap.class_name.list
        )
        v.virtual_list(
            key="event-list",
            item_count=len(self.rows),
            item_size=28.0 if self._is_decius() else 40.0,
            visible_items=8,
            overscan=1,
            classes=list_classes,
            build=self._build_row,
        )
        v.paragraph("Scene tree", classes=note_class, key="tree-prompt")
        tree_classes = (
            f"aui-tree-list {ui.decius.class_name.tree}"
            if self._is_decius()
            else f"aui-tree-list {ui.bootstrap.class_name.tree}"
        )
        v.container(classes=tree_classes, key="scene-tree", build=self._build_tree_rows)

    def _build_tab_body(self, v: ui.View) -> None:
        if self.active_tab == "fields":
            self._build_fields(v)
        elif self.active_tab == "collections":
            self._build_collections(v)
        else:
            self._build_controls(v)

    def _tab_body_class(self) -> str:
        if self.active_tab == "fields":
            return self._flow_class(self.field_layout)
        if self.active_tab == "controls":
            return self._flow_class("form")
        return (
            ui.decius.class_name.column
            if self._is_decius()
            else ui.bootstrap.class_name.column
        )

    def _build_panel(self, v: ui.View) -> None:
        v.heading(1, "Hello from AffineUI", key="title")
        v.paragraph(
            "The same Python command tree is rendered with Bootstrap or Decius selectors.",
            key="lede",
        )
        tabs_class = "dcs-tabs" if self._is_decius() else "nav nav-tabs"
        v.container(classes=tabs_class, key="tabs", build=self._build_tabs)
        v.container(
            classes=self._tab_body_class(),
            key="tab-body",
            build=self._build_tab_body,
        )

    def build_view(self) -> ui.View:
        view = ui.View(self.theme)
        view.begin()
        view.panel(key="hello-panel", build=self._build_panel)
        view.end()
        return view

    def reload(self) -> None:
        if self.app is not None:
            self.app.load_view(self.build_view())

    def select_tab(self, tab: str) -> None:
        self.active_tab = tab
        self.reload()

    def set_field_layout(self, layout: str) -> None:
        if layout not in ("form", "props"):
            return
        self.field_layout = layout
        self.reload()


def build_view(theme: ui.ViewTheme) -> ui.View:
    return HelloPanel(theme).build_view()


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--headless",
        action="store_true",
        help="Build and lay out the panel without opening a window.",
    )
    parser.add_argument(
        "--style",
        choices=("bootstrap", "decius"),
        default="bootstrap",
        help="Framework selector set to use.",
    )
    args = parser.parse_args()

    theme = parse_style(args.style)
    controller = HelloPanel(theme)
    app = ui.App(
        title=f"AffineUI Python Hello Panel ({args.style})",
        width=720,
        height=520,
        asset_folders=["examples"],
    )
    controller.app = app
    app.load_view(controller.build_view())

    if args.headless:
        app.document().layout(720, 520)
        size = app.document().content_size()
        print(f"laid out {size.width}x{size.height}")
        return

    app.launch(native=True)


if __name__ == "__main__":
    main()
