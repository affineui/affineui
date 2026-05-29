import argparse
import sys
from pathlib import Path
from typing import Callable, NamedTuple, Optional

import affineui as ui

EXAMPLE_DIR = Path(__file__).resolve().parent
if str(EXAMPLE_DIR) not in sys.path:
    sys.path.insert(0, str(EXAMPLE_DIR))

from test_panels import collections, controls, decius_reference, forms, overview, photo


PanelBuilder = Callable[["ComponentGalleryApp", ui.View], None]


class NavItem(NamedTuple):
    key: str
    label: str
    description: str
    icon: str
    builder: Optional[PanelBuilder]


class NavGroup(NamedTuple):
    label: str
    icon: str
    items: tuple[NavItem, ...]


def parse_style(name: str) -> ui.ViewTheme:
    normalized = name.strip().lower()
    if normalized == "bootstrap":
        return ui.ViewTheme.Bootstrap
    if normalized == "decius":
        return ui.ViewTheme.Decius
    raise ValueError(f"unknown theme: {name}")


class ComponentGalleryApp:
    def __init__(self, theme: ui.ViewTheme) -> None:
        self.theme = theme
        self.app: Optional[ui.App] = None
        self.active_page = "overview"
        self.field_layout = "split"
        self.visual_style = "flat"
        self.density = "compact"
        self.accent = "cyan"
        self.rows = [f"Frame {index:02d} - event routed" for index in range(1, 49)]
        self.selected_row = 0
        self.selected_tree = "tree-camera"
        self.photo_tool = "brush"
        self.photo_layer = "layer-hero"
        self.nav_groups: tuple[NavGroup, ...] = (
            NavGroup(
                "Workbench",
                "decius",
                (
                    NavItem(
                        "overview",
                        "Overview",
                        "What this test bench covers",
                        "decius",
                        overview.build,
                    ),
                ),
            ),
            NavGroup(
                "Widget Surface",
                "gizmo",
                (
                    NavItem(
                        "controls",
                        "Controls",
                        "Buttons, toggles, sliders, knobs",
                        "check-circle",
                        controls.build,
                    ),
                    NavItem(
                        "forms",
                        "Forms & Props",
                        "Inputs, editors, form and property flows",
                        "gain",
                        forms.build,
                    ),
                    NavItem(
                        "photo",
                        "Photo Hybrid",
                        "Decius photo editor shell driven from Python on the C++ runtime",
                        "image",
                        photo.build,
                    ),
                    NavItem(
                        "collections",
                        "Lists & Tree",
                        "Scrollable rows, trees, and stable selection",
                        "folder-open",
                        collections.build,
                    ),
                ),
            ),
            NavGroup(
                "Decius CSS Matrix",
                "palette",
                tuple(
                    NavItem(key, label, description, icon, builder)
                    for key, label, description, icon, builder in decius_reference.page_specs()
                ),
            ),
            NavGroup(
                "Planned Coverage",
                "rocket",
                (
                    NavItem(
                        "typography",
                        "Typography",
                        "Text scales and rich copy",
                        "pencil",
                        None,
                    ),
                    NavItem(
                        "animation",
                        "Animation",
                        "Motion and invalidation",
                        "keyframe",
                        None,
                    ),
                ),
            ),
        )
        self.pages = tuple(
            item
            for group in self.nav_groups
            for item in group.items
            if item.builder is not None
        )

    def is_decius(self) -> bool:
        return self.theme == ui.ViewTheme.Decius

    def note_class(self) -> str:
        return (
            ui.decius.class_name.note
            if self.is_decius()
            else ui.bootstrap.class_name.note
        )

    def flow_class(self, mode: str) -> str:
        if mode not in ("form", "props"):
            return ""
        if self.is_decius():
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

    def is_decius_flat(self) -> bool:
        return self.is_decius() and self.visual_style == "flat"

    def section_class(self, mode: str) -> str:
        if self.is_decius_flat():
            return "aui-demo-section dcs-subpanel"
        if self.is_decius():
            return "aui-demo-section dcs-panel dcs-panel--bordered dcs-panel--raised"
        return "aui-demo-section card shadow-sm"

    def section_header_class(self) -> str:
        if self.is_decius_flat():
            return "dcs-subpanel__header"
        return "dcs-panel__header" if self.is_decius() else "card-header h6 mb-0"

    def section_body_class(self, mode: str) -> str:
        flow_class = self.flow_class(mode)
        if self.is_decius_flat():
            return "dcs-subpanel__body" + (f" {flow_class}" if flow_class else "")
        if self.is_decius():
            return "dcs-panel__body" + (f" {flow_class}" if flow_class else "")
        return "card-body" + (f" {flow_class}" if flow_class else "")

    def section(self, v: ui.View, title: str, mode: str, key: str, build) -> None:
        def build_panel(panel: ui.View) -> None:
            panel.paragraph(title, classes=self.section_header_class(), key=f"{key}-header")
            panel.container(
                classes=self.section_body_class(mode),
                key=f"{key}-body",
                build=build,
            )

        v.container(classes=self.section_class(mode), key=key, build=build_panel)

    def page_label(self, page_key: str) -> str:
        for item in self.pages:
            if item.key == page_key:
                return item.label
        return self.pages[0].label

    def _page_from_label(self, label: str) -> str:
        for item in self.pages:
            if item.label == label:
                return item.key
        return self.active_page

    def _active_page(self) -> NavItem:
        for item in self.pages:
            if item.key == self.active_page:
                return item
        return self.pages[0]

    def _icon_class(self, icon: str) -> str:
        return f"aui-test-nav-icon di di-{icon}"

    def _build_nav_item(self, v: ui.View, item: NavItem) -> None:
        active = item.key == self.active_page
        classes = (
            "aui-test-nav-item"
            + (" is-active" if active else "")
            + (" is-disabled" if item.builder is None else "")
        )
        def build_item(row: ui.View) -> None:
            row.paragraph(
                "",
                classes=self._icon_class(item.icon),
                key=f"nav-{item.key}-icon",
            )
            row.paragraph(
                item.label,
                classes="aui-test-nav-label",
                key=f"nav-{item.key}-label",
            )

        nav_item = v.container(classes=classes, key=f"nav-{item.key}", build=build_item)
        nav_item.attr("role", "button")
        nav_item.attr("title", item.description)
        nav_item.attr("aria-current", "page" if active else "false")
        if item.builder is None:
            nav_item.attr("aria-disabled", "true")
            nav_item.attr("tabindex", "-1")
        else:
            nav_item.on_click(lambda page=item.key: self.select_page(page))

    def _build_nav_group(self, v: ui.View, group: NavGroup) -> None:
        def build_group(group_view: ui.View) -> None:
            group_key = group.label.lower().replace(" ", "-")

            def build_title(title: ui.View) -> None:
                title.paragraph(
                    "",
                    classes=self._icon_class(group.icon),
                    key=f"nav-group-title-{group_key}-icon",
                )
                title.paragraph(
                    group.label,
                    classes="aui-test-nav-label",
                    key=f"nav-group-title-{group_key}-label",
                )

            group_view.container(
                classes="aui-test-nav-group-title",
                key=f"nav-group-title-{group_key}",
                build=build_title,
            )
            for item in group.items:
                self._build_nav_item(group_view, item)

        key = "nav-group-" + group.label.lower().replace(" ", "-")
        v.container(classes="aui-test-nav-group", key=key, build=build_group)

    def _build_desktop_nav(self, v: ui.View) -> None:
        v.paragraph("Test Index", classes="aui-test-nav-heading", key="nav-label")
        for group in self.nav_groups:
            self._build_nav_group(v, group)

    def _build_mobile_nav(self, v: ui.View) -> None:
        labels = [item.label for item in self.pages]
        v.dropdown(
            "Test area",
            labels,
            self.page_label(self.active_page),
            key="mobile-nav",
        ).on_change(lambda label: self.select_page(self._page_from_label(label)))

    def _segment_group_class(self) -> str:
        if self.is_decius():
            return "aui-test-segment-group dcs-btn-group"
        return "aui-test-segment-group btn-group"

    def _segment_button_class(self, active: bool) -> str:
        if self.is_decius():
            return "aui-test-segment dcs-btn" + (" dcs-btn--primary" if active else "")
        return "aui-test-segment btn " + ("btn-primary" if active else "btn-outline-secondary")

    def _build_segment_group(
        self,
        v: ui.View,
        label: str,
        options: tuple[tuple[str, str], ...],
        selected: str,
        key: str,
        on_select: Callable[[str], None],
    ) -> None:
        def build_control(control: ui.View) -> None:
            control.paragraph(label, classes="aui-test-control-label", key=f"{key}-label")

            def build_group(group: ui.View) -> None:
                for option_label, option_value in options:
                    active = option_value == selected
                    button = group.button(
                        option_label,
                        primary=active,
                        key=f"{key}-{option_value}",
                    )
                    button.cls(self._segment_button_class(active))
                    button.attr("aria-pressed", "true" if active else "false")
                    button.on_click(lambda value=option_value: on_select(value))

            control.container(
                classes=self._segment_group_class(),
                key=f"{key}-group",
                build=build_group,
            ).attr("role", "group")

        v.container(classes="aui-test-control", key=key, build=build_control)

    def _build_selector_controls(self, v: ui.View) -> None:
        if self.is_decius():
            self._build_segment_group(
                v,
                "Style",
                (("Flat", "flat"), ("3D", "3d")),
                self.visual_style,
                "top-style",
                self.set_visual_style,
            )
            self._build_segment_group(
                v,
                "Density",
                (
                    ("Compact", "compact"),
                    ("Comfort", "comfortable"),
                    ("Spacious", "spacious"),
                ),
                self.density,
                "top-density",
                self.set_density,
            )
            self._build_key_color_controls(v)

    def _build_key_color_controls(self, v: ui.View) -> None:
        def build_group(group: ui.View) -> None:
            group.paragraph("Key", classes="aui-keycolor-label", key="keycolor-label")
            for accent in ("cyan", "green", "orange", "violet"):
                active = accent == self.accent
                swatch = group.container(
                    classes=(
                        f"aui-keycolor-swatch aui-keycolor-swatch--{accent}"
                        + (" is-active" if active else "")
                    ),
                    key=f"top-accent-{accent}",
                )
                swatch.attr("role", "button")
                swatch.attr("aria-label", f"{accent} key color")
                swatch.attr("aria-pressed", "true" if active else "false")
                swatch.attr("title", f"{accent.title()} key color")
                swatch.on_click(lambda value=accent: self.set_accent(value))

        v.container(classes="aui-test-keycolor", key="top-accent", build=build_group)

    def _topbar_class(self) -> str:
        if self.is_decius():
            return "aui-test-topbar dcs-menubar"
        return "aui-test-topbar navbar navbar-expand bg-body-tertiary border-bottom"

    def _brand_class(self) -> str:
        return "aui-test-brand dcs-menubar__brand" if self.is_decius() else "aui-test-brand navbar-brand"

    def _meta_class(self) -> str:
        return "aui-test-subtitle dcs-menubar__meta" if self.is_decius() else "aui-test-subtitle text-body-secondary"

    def _page_panel_class(self) -> str:
        return "aui-test-page"

    def _page_panel_header_class(self) -> str:
        return "aui-test-page-header"

    def _page_panel_body_class(self) -> str:
        return "aui-test-page-body"

    def _build_header(self, v: ui.View) -> None:
        def build_brand(brand: ui.View) -> None:
            brand.paragraph("", classes="aui-test-brand-icon di di-decius", key="app-icon")
            brand.heading(1, "AffineUI Test Bench", key="app-title")

        v.container(classes=self._brand_class(), key="test-brand", build=build_brand)
        v.container(
            classes="aui-test-tweaks",
            key="test-tweaks",
            build=self._build_selector_controls,
        )
        v.container(classes="aui-test-perf-reserve", key="perf-reserve")

    def _build_active_test(self, v: ui.View) -> None:
        page = self._active_page()
        if page.builder is not None:
            def build_page(panel: ui.View) -> None:
                panel.container(
                    classes=self._page_panel_body_class(),
                    key=f"{page.key}-panel-body",
                    build=lambda body: self._build_page_body(page, body),
                )

            v.container(
                classes=self._page_panel_class(),
                key=f"{page.key}-panel",
                build=build_page,
            )

    def _build_page_body(self, page: NavItem, body: ui.View) -> None:
        body.heading(
            1,
            page.label,
            classes=self._page_panel_header_class(),
            key=f"{page.key}-panel-header",
        )
        page.builder(self, body)

    def _build_body(self, v: ui.View) -> None:
        v.container(
            classes="aui-test-mobile-nav",
            key="mobile-nav-shell",
            build=self._build_mobile_nav,
        )

        def build_body(body: ui.View) -> None:
            body.container(
                classes="aui-test-nav",
                key="desktop-nav",
                build=self._build_desktop_nav,
            )
            body.container(
                classes="aui-test-content",
                key="test-content",
                build=self._build_active_test,
            )

        v.container(classes="aui-test-body", key="test-body", build=build_body)

    def _build_shell(self, v: ui.View) -> None:
        v.container(
            classes=self._topbar_class(),
            key="test-header",
            build=self._build_header,
        )
        v.container(
            classes="aui-test-shell-inner",
            key="test-shell-inner",
            build=self._build_body,
        )

    def build_view(self) -> ui.View:
        view = ui.View(self.theme)
        if self.is_decius():
            view.selector(ui.decius.selector.style, self.visual_style)
            view.selector(ui.decius.selector.density, self.density)
            view.selector(ui.decius.selector.accent, self.accent)
        view.begin()
        view.container(
            classes="aui-test-shell",
            key="test-shell",
            build=self._build_shell,
        )
        view.end()
        return view

    def reload(self) -> None:
        if self.app is not None:
            self.app.load_view(self.build_view())

    def select_page(self, page: str) -> None:
        if page not in {item.key for item in self.pages}:
            return
        self.active_page = page
        self.reload()

    def set_field_layout(self, layout: str) -> None:
        if layout not in ("split", "form", "props"):
            return
        self.field_layout = layout
        self.reload()

    def set_visual_style(self, value: str) -> None:
        normalized = "3d" if value.strip().lower() in ("3d", "synth") else "flat"
        self.visual_style = normalized
        self.reload()

    def set_density(self, density: str) -> None:
        if density == "comfort":
            density = "comfortable"
        if density not in ("compact", "comfortable", "spacious"):
            return
        self.density = density
        self.reload()

    def set_accent(self, accent: str) -> None:
        if accent not in ("cyan", "green", "orange", "violet"):
            return
        self.accent = accent
        self.reload()

    def select_row(self, index: int) -> None:
        self.selected_row = index
        self.reload()

    def append_row(self) -> None:
        self.rows.append(f"Generated row {len(self.rows) + 1:02d}")
        self.selected_row = len(self.rows) - 1
        self.reload()

    def build_list_rows(self, v: ui.View) -> None:
        list_classes = (
            ui.decius.class_name.list
            if self.is_decius()
            else ui.bootstrap.class_name.list
        )
        v.virtual_list(
            key="event-list",
            item_count=len(self.rows),
            item_size=28.0 if self.is_decius() else 40.0,
            visible_items=10,
            overscan=2,
            classes=list_classes,
            build=self.build_row,
        )

    def build_row(self, v: ui.View, index: int) -> None:
        title = self.rows[index]
        selected = index == self.selected_row
        row = v.button(title, primary=selected, key=f"log-row-{index}")
        row.on_click(lambda current_index=index: self.select_row(current_index))
        if self.is_decius():
            row.cls(ui.decius.class_name.list_item).attr(
                "aria-selected", "true" if selected else "false"
            )
        else:
            row.cls(
                f"{ui.bootstrap.class_name.list_item} list-group-item-action"
                + (" active" if selected else "")
            )

    def select_tree(self, key: str) -> None:
        self.selected_tree = key
        self.reload()

    def set_photo_tool(self, tool: str) -> None:
        self.photo_tool = tool
        self.reload()

    def set_photo_layer(self, layer: str) -> None:
        self.photo_layer = layer
        self.reload()

    def tree_row(
        self,
        v: ui.View,
        label: str,
        key: str,
        depth: int = 0,
        branch: bool = False,
        expanded: bool = True,
    ) -> None:
        selected = self.selected_tree == key
        row = v.button(label, primary=selected, key=key)
        row.on_click(lambda current_key=key: self.select_tree(current_key))
        indent = 12 + depth * 18
        tree_state_class = "aui-tree-branch" if branch else "aui-tree-leaf"
        if self.is_decius():
            row.cls(
                f"aui-tree-row {tree_state_class} {ui.decius.class_name.tree_row}"
            ).attr("aria-selected", "true" if selected else "false")
        else:
            row.cls(
                f"aui-tree-row {tree_state_class} "
                f"{ui.bootstrap.class_name.tree_row} list-group-item-action"
                + (" active" if selected else "")
            )
        if branch:
            row.attr("aria-expanded", "true" if expanded else "false")
        row.attr("style", f"width:100%;padding-left:{indent}px")

    def build_tree_rows(self, v: ui.View) -> None:
        self.tree_row(v, "Scene", "tree-scene", 0, branch=True)
        self.tree_row(v, "Camera", "tree-camera", 1)
        self.tree_row(v, "Key Light", "tree-light", 1)
        self.tree_row(v, "Player", "tree-player", 1, branch=True)
        self.tree_row(v, "Mesh", "tree-player-mesh", 2)
        self.tree_row(v, "Controller", "tree-player-controller", 2)
        self.tree_row(v, "Environment", "tree-environment", 1, branch=True)
        self.tree_row(v, "Collision", "tree-collision", 2)
        self.tree_row(v, "Audio", "tree-audio", 1, branch=True)
        self.tree_row(v, "Music", "tree-music", 2)
        self.tree_row(v, "Foley", "tree-foley", 2)
        self.tree_row(v, "UI", "tree-ui", 1, branch=True)
        self.tree_row(v, "HUD", "tree-hud", 2)
        self.tree_row(v, "Inventory", "tree-inventory", 2)
        self.tree_row(v, "Simulation", "tree-simulation", 1, branch=True)
        self.tree_row(v, "Navigation", "tree-navigation", 2)
        self.tree_row(v, "Physics", "tree-physics", 2)
        self.tree_row(v, "Networking", "tree-networking", 1, branch=True)
        self.tree_row(v, "Replication", "tree-replication", 2)


AffineUITestApp = ComponentGalleryApp
HelloPanel = ComponentGalleryApp


def build_view(theme: ui.ViewTheme) -> ui.View:
    return ComponentGalleryApp(theme).build_view()


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
    parser.add_argument(
        "--perf",
        action="store_true",
        help="Show the native performance overlay. Click it to move corners.",
    )
    parser.add_argument(
        "--dpi",
        choices=("retina", "normal"),
        default="retina",
        help="Native window DPI mode. retina uses the platform high-DPI framebuffer; normal uses 1x.",
    )
    args = parser.parse_args()

    theme = parse_style(args.style)
    controller = ComponentGalleryApp(theme)
    app = ui.App(
        title=f"AffineUI Component Gallery ({args.style})",
        width=1024,
        height=680,
        asset_folders=["examples"],
        perf_overlay=args.perf,
        high_dpi=args.dpi == "retina",
    )
    controller.app = app
    app.load_view(controller.build_view())

    if args.headless:
        app.document().layout(1024, 680)
        size = app.document().content_size()
        print(f"laid out {size.width}x{size.height}")
        return

    app.launch(native=True)


if __name__ == "__main__":
    main()
