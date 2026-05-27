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
        self.rows = [
            "Renderer warm",
            "Input route armed",
            "Patch graph clean",
        ]
        self.selected_row = 0

    def _is_decius(self) -> bool:
        return self.theme == ui.ViewTheme.Decius

    def _tab_button(self, v: ui.View, label: str, tab: str) -> None:
        active = self.active_tab == tab
        button = v.button(label, primary=active, key=f"tab-{tab}")
        button.on_click(lambda tab=tab: self.select_tab(tab))
        if self._is_decius():
            button.cls("dcs-tab").attr("aria-selected", "true" if active else "false")
        else:
            button.cls("btn btn-primary" if active else "btn btn-outline-secondary")

    def _build_tabs(self, v: ui.View) -> None:
        self._tab_button(v, "Controls", "controls")
        self._tab_button(v, "Fields", "fields")
        self._tab_button(v, "List", "list")

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
        v.input("Object name", "Cylinder.042", key="object-name")
        v.password("Token", "secret", key="token")
        v.input("Gain", "1.000", type="number", key="gain-field")
        v.dropdown(
            "Mode",
            ["Object", "Edit", "Sculpt", "Render"],
            "Object",
            key="mode",
        )
        v.button_group(
            "Transform space",
            ["Local", "World", "View"],
            "World",
            key="space",
        )
        v.textarea(
            "Notes",
            "Dense native UI, browser semantics.",
            rows=3,
            key="notes",
        )

    def _select_row(self, index: int) -> None:
        self.selected_row = index
        self.reload()

    def _append_row(self) -> None:
        self.rows.append(f"Generated row {len(self.rows) + 1:02d}")
        self.selected_row = len(self.rows) - 1
        self.reload()

    def _build_rows(self, v: ui.View) -> None:
        for index, title in enumerate(self.rows):
            selected = index == self.selected_row
            row = v.button(title, primary=selected, key=f"log-row-{index}")
            row.on_click(lambda index=index: self._select_row(index))
            if self._is_decius():
                row.cls("dcs-card dcs-card--clickable").attr(
                    "aria-selected", "true" if selected else "false"
                )
            else:
                row.cls(
                    "list-group-item list-group-item-action"
                    + (" active" if selected else "")
                )

    def _build_list(self, v: ui.View) -> None:
        v.button("Append row", primary=True, key="append-row").on_click(self._append_row)
        list_classes = "dcs-card-list" if self._is_decius() else "list-group"
        v.container(classes=list_classes, key="event-list", build=self._build_rows)

    def _build_tab_body(self, v: ui.View) -> None:
        if self.active_tab == "fields":
            self._build_fields(v)
        elif self.active_tab == "list":
            self._build_list(v)
        else:
            self._build_controls(v)

    def _build_panel(self, v: ui.View) -> None:
        v.heading(1, "Hello from AffineUI", key="title")
        v.paragraph(
            "The same Python command tree is rendered with Bootstrap or Decius selectors.",
            key="lede",
        )
        tabs_class = "dcs-tabs" if self._is_decius() else "btn-group"
        v.container(classes=tabs_class, key="tabs", build=self._build_tabs)
        body_class = "dcs-col" if self._is_decius() else "d-flex flex-column gap-3"
        v.container(classes=body_class, key="tab-body", build=self._build_tab_body)

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
