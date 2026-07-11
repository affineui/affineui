import argparse
import sys
from pathlib import Path
from typing import Callable, NamedTuple, Optional

import affineui as ui

EXAMPLE_DIR = Path(__file__).resolve().parent
if str(EXAMPLE_DIR) not in sys.path:
    sys.path.insert(0, str(EXAMPLE_DIR))

from test_panels import collections, controls, decius_reference, forms, photo


def asset_root() -> str:
    """Absolute path to the assets directory that holds ``frameworks/`` (the
    framework CSS bundles the gallery renders against).

    Resolved from this file's own location, never the current working
    directory, so the gallery loads its stylesheets no matter where it is
    launched from — running it should always just work. We walk up from here
    looking for a directory that contains ``frameworks/css``; the repo layout
    puts it at ``<repo>/examples``. Falls back to this file's own directory so
    a self-contained/installed copy that ships its own ``frameworks/`` beside
    the script keeps working.
    """
    for base in (EXAMPLE_DIR, *EXAMPLE_DIR.parents):
        candidate = base / "examples"
        if (candidate / "frameworks" / "css").is_dir():
            return str(candidate)
        if (base / "frameworks" / "css").is_dir():
            return str(base)
    return str(EXAMPLE_DIR)


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
        self.active_page = "controls"
        self.field_layout = "split"
        self.visual_style = "flat"
        self.density = "compact"
        self.accent = "cyan"
        # A large synthetic list to exercise the recycling virtual list: only
        # the visible rows are ever built, so 50k rows scroll smoothly.
        self.row_count = 50000
        self.list_selection = ui.IndexSelection()
        self.list_selection.on_change(self.reload)
        self.list_provider = ui.VirtualListProvider()
        # Row height tracks the UI density mode (decius --dcs-h). Row height
        # is exact now (inline flex-basis pins it), so this is the height
        # rows RENDER at; set_density() re-applies it.
        self.list_provider.default_item_size(self.dcs_row_height())
        self.list_provider.on_item_count(lambda: self.row_count)
        self.list_provider.on_item_text(
            lambda i: f"Frame {i:05d} - event routed"
        )
        self.list_provider.on_is_selected(
            lambda i: self.list_selection.contains(i)
        )
        self.list_provider.on_activate(
            lambda i, mod: self.list_selection.apply(i, mod, self.row_count)
        )
        # Checked is a SECOND, independent row state (list indices are stable
        # identities here, so an index set is fine; trees key by node id).
        self.collections_checkboxes = False
        self._list_checked: set[int] = set()
        self.list_provider.on_is_checked(lambda i: i in self._list_checked)
        self.list_provider.on_set_checked(self._set_list_checked)

        # A virtual tree over a small scene graph. Nodes carry (label, depth,
        # id); `_tree_expanded` holds the ids of open branches. The provider
        # answers everything from the flattened-visible view, which is recomputed
        # whenever a branch opens/closes.
        self._tree_nodes = self._build_scene_nodes()
        self._tree_expanded = {
            n["id"] for n in self._tree_nodes if n["branch"]
        }
        self._tree_flat: list[dict] = []
        self._reflatten_tree()
        # Tree selection is keyed by node ID (the handle), NEVER by flattened
        # index: expanding/collapsing renumbers rows, and an index-keyed
        # selection would silently migrate onto different items. IDs are
        # stable identities unique to each node.
        self._tree_selected: set[str] = set()
        self._tree_sel_anchor: str | None = None
        self.tree_provider = ui.VirtualTreeProvider()
        self.tree_provider.default_item_size(self.dcs_row_height() + 2.0)
        self.tree_provider.on_item_count(lambda: len(self._tree_flat))
        self.tree_provider.on_item_text(lambda i: self._tree_flat[i]["label"])
        self.tree_provider.on_depth(lambda i: self._tree_flat[i]["depth"])
        self.tree_provider.on_is_expandable(
            lambda i: self._tree_flat[i]["branch"]
        )
        self.tree_provider.on_is_expanded(
            lambda i: self._tree_flat[i]["id"] in self._tree_expanded
        )
        self.tree_provider.on_toggle(self._toggle_tree_node)
        self.tree_provider.on_is_selected(
            lambda i: self._tree_flat[i]["id"] in self._tree_selected
        )
        self.tree_provider.on_activate(self._activate_tree_row)
        # Checked state keyed by node ID — like selection, it must survive
        # expand/collapse renumbering.
        self._tree_checked: set[str] = set()
        self.tree_provider.on_is_checked(
            lambda i: self._tree_flat[i]["id"] in self._tree_checked
        )
        self.tree_provider.on_set_checked(self._set_tree_checked)
        self.photo_tool = "brush"
        self.photo_layer = "layer-hero"
        self.nav_groups: tuple[NavGroup, ...] = (
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
                        "Photo Edit",
                        "Pure-Python Decius photo editor surface",
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

        v.container(
            classes=f"aui-test-control aui-test-control--{key}",
            key=key,
            build=build_control,
        )

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

        v.container(
            classes="aui-test-keycolor aui-test-control--top-accent",
            key="top-accent",
            build=build_group,
        )

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

    def build_into(self, view: ui.View) -> None:
        """Fill a framework-owned view. Used with App.set_view so the
        reconcile fast path drives rebuilds — required for the virtual list/
        tree to follow the scrollbar as the user scrolls."""
        view.set_theme(self.theme)
        if self.is_decius():
            view.selector(ui.decius.selector.style, self.visual_style)
            view.selector(ui.decius.selector.density, self.density)
            view.selector(ui.decius.selector.accent, self.accent)
        view.container(
            classes="aui-test-shell",
            key="test-shell",
            build=self._build_shell,
        )

    def reload(self) -> None:
        if self.app is not None:
            self.app.rebuild_view()

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

    def dcs_row_height(self) -> float:
        """List/tree row height for the active density (decius --dcs-h)."""
        return {"compact": 20.0, "comfortable": 24.0, "spacious": 28.0}.get(
            self.density, 24.0)

    def set_density(self, density: str) -> None:
        if density == "comfort":
            density = "comfortable"
        if density not in ("compact", "comfortable", "spacious"):
            return
        self.density = density
        # Virtual rows are exact-height; keep them in step with --dcs-h.
        self.list_provider.default_item_size(self.dcs_row_height())
        self.tree_provider.default_item_size(self.dcs_row_height() + 2.0)
        self.reload()

    def set_accent(self, accent: str) -> None:
        if accent not in ("cyan", "green", "orange", "violet"):
            return
        self.accent = accent
        self.reload()

    # ── Virtual list panel (provider-backed) ────────────────────────────────

    def build_list_rows(self, v: ui.View) -> None:
        list_classes = (
            ui.decius.class_name.list
            if self.is_decius()
            else ui.bootstrap.class_name.list
        )
        # Recycling virtual list: only the visible rows are built, so the 50k
        # rows scroll smoothly and selection is re-derived from the model each
        # rebuild. Ctrl/Cmd toggles, Shift extends the range.
        v.virtual_list(key="event-list", provider=self.list_provider,
                       classes=list_classes)

    def set_photo_tool(self, tool: str) -> None:
        self.photo_tool = tool
        self.reload()

    def set_photo_layer(self, layer: str) -> None:
        self.photo_layer = layer
        self.reload()

    # ── Virtual tree panel (provider-backed) ────────────────────────────────

    @staticmethod
    def _build_scene_nodes() -> list[dict]:
        """A small scene graph as a flat list of (id, label, depth, branch)."""
        spec = [
            ("scene", "Scene", 0, True),
            ("camera", "Camera", 1, False),
            ("light", "Key Light", 1, False),
            ("player", "Player", 1, True),
            ("player-mesh", "Mesh", 2, False),
            ("player-controller", "Controller", 2, False),
            ("environment", "Environment", 1, True),
            ("collision", "Collision", 2, False),
            ("audio", "Audio", 1, True),
            ("music", "Music", 2, False),
            ("foley", "Foley", 2, False),
            ("ui", "UI", 1, True),
            ("hud", "HUD", 2, False),
            ("inventory", "Inventory", 2, False),
            ("simulation", "Simulation", 1, True),
            ("navigation", "Navigation", 2, False),
            ("physics", "Physics", 2, False),
            ("networking", "Networking", 1, True),
            ("replication", "Replication", 2, False),
        ]
        return [
            {"id": nid, "label": label, "depth": depth, "branch": branch}
            for nid, label, depth, branch in spec
        ]

    def _reflatten_tree(self) -> None:
        """Recompute the visible node list: a node is shown when every ancestor
        branch is expanded. (Depth encodes the tree shape for this flat spec.)"""
        flat: list[dict] = []
        # Track, per depth, whether the current branch chain is fully open.
        open_at_depth = [True]
        for node in self._tree_nodes:
            depth = node["depth"]
            # Trim the open-chain stack to this node's parent depth.
            del open_at_depth[depth + 1 :]
            parent_open = open_at_depth[depth]
            if parent_open:
                flat.append(node)
            expanded = parent_open and (node["id"] in self._tree_expanded)
            # Push this node's open state for its children.
            if len(open_at_depth) == depth + 1:
                open_at_depth.append(expanded)
            else:
                open_at_depth[depth + 1] = expanded
        self._tree_flat = flat

    def _set_list_checked(self, index: int, on: bool) -> None:
        if on:
            self._list_checked.add(index)
        else:
            self._list_checked.discard(index)
        self.reload()

    def _set_tree_checked(self, index: int, on: bool) -> None:
        nid = self._tree_flat[index]["id"]
        if on:
            self._tree_checked.add(nid)
        else:
            self._tree_checked.discard(nid)
        self.reload()

    def set_collections_checkboxes(self, on: bool) -> None:
        """Set checkbox mode for both the virtual list and the virtual tree."""
        self.collections_checkboxes = on
        self.list_provider.checkboxes(on)
        self.tree_provider.checkboxes(on)
        self.reload()

    def _activate_tree_row(self, index: int, mod: "ui.SelectMod") -> None:
        """Plain / Ctrl (toggle) / Shift (range) selection, keyed by node ID.
        Range selects the IDs currently visible between the anchor and the
        clicked row."""
        nid = self._tree_flat[index]["id"]
        if mod == ui.SelectMod.Toggle:
            if nid in self._tree_selected:
                self._tree_selected.discard(nid)
            else:
                self._tree_selected.add(nid)
            self._tree_sel_anchor = nid
        elif mod == ui.SelectMod.Range:
            anchor_index = index
            if self._tree_sel_anchor is not None:
                for k, node in enumerate(self._tree_flat):
                    if node["id"] == self._tree_sel_anchor:
                        anchor_index = k
                        break
            lo, hi = sorted((anchor_index, index))
            self._tree_selected = {
                node["id"] for node in self._tree_flat[lo : hi + 1]
            }
            if self._tree_sel_anchor is None:
                self._tree_sel_anchor = nid
        else:  # Replace
            self._tree_selected = {nid}
            self._tree_sel_anchor = nid
        self.reload()

    def _toggle_tree_node(self, index: int) -> None:
        node = self._tree_flat[index]
        nid = node["id"]
        if nid in self._tree_expanded:
            self._tree_expanded.discard(nid)
        else:
            self._tree_expanded.add(nid)
        self._reflatten_tree()
        self.reload()

    def build_tree_rows(self, v: ui.View) -> None:
        v.virtual_tree(key="scene-tree", provider=self.tree_provider)


AffineUITestApp = ComponentGalleryApp
HelloPanel = ComponentGalleryApp


def build_view(theme: ui.ViewTheme) -> ui.View:
    """Build a one-shot View (for headless/snapshot callers). Interactive use
    should prefer App.set_view(controller.build_into) so virtual lists follow
    the scrollbar."""
    controller = ComponentGalleryApp(theme)
    view = ui.View(theme)
    view.begin()
    controller.build_into(view)
    view.end()
    return view


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
        default="decius",
        help="Framework selector set to use. Decius is the default and the "
             "fully-supported personality; the Bootstrap set does not yet "
             "carry CSS for every AffineUI widget.",
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
    parser.add_argument(
        "--page",
        default="controls",
        help="Initial gallery page key, for example: controls, forms, photo.",
    )
    args = parser.parse_args()

    theme = parse_style(args.style)
    controller = ComponentGalleryApp(theme)
    page_keys = {page.key for page in controller.pages}
    if args.page not in page_keys:
        parser.error(f"unknown page '{args.page}'. Available: {', '.join(sorted(page_keys))}")
    controller.active_page = args.page
    app = ui.App(
        title=f"AffineUI Component Gallery ({args.style})",
        width=1024,
        height=680,
        asset_folders=[asset_root()],
        perf_overlay=args.perf,
        high_dpi=args.dpi == "retina",
    )
    controller.app = app
    # set_view registers a persistent builder and uses the reconcile fast path,
    # so recycling virtual lists/trees follow the scrollbar automatically.
    app.set_view(controller.build_into)

    if args.headless:
        app.document().layout(1024, 680)
        size = app.document().content_size()
        print(f"laid out {size.width}x{size.height}")
        return

    app.launch(native=True)


if __name__ == "__main__":
    main()
