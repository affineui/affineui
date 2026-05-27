import argparse

import affineui as ui


def parse_style(name: str) -> ui.ViewTheme:
    normalized = name.strip().lower()
    if normalized == "bootstrap":
        return ui.ViewTheme.Bootstrap
    if normalized == "decius":
        return ui.ViewTheme.Decius
    raise ValueError(f"unknown theme: {name}")


def build_view(theme: ui.ViewTheme) -> ui.View:
    view = ui.View(theme)

    view.begin()
    panel = view.panel(key="hello-panel")
    view.end()

    panel.replace(
        lambda v: (
            v.heading(1, "Hello from AffineUI", key="title"),
            v.paragraph(
                "This panel is built from Python commands and rendered by "
                "the native AffineUI engine using framework selectors.",
                key="lede",
            ),
            v.button("Hello button", primary=True, key="hello-button"),
            v.checkbox("Framework checkbox", checked=True, key="hello-check"),
            v.slider(
                "Framework slider",
                value=0.65,
                min=0.0,
                max=1.0,
                key="hello-slider",
            ),
        )
    )
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
        default="bootstrap",
        help="Framework selector set to use.",
    )
    args = parser.parse_args()

    theme = parse_style(args.style)
    app = ui.App(
        title=f"AffineUI Python Hello Panel ({args.style})",
        width=720,
        height=420,
        asset_folders=["examples"],
    )
    app.load_view(build_view(theme))

    if args.headless:
        app.document().layout(720, 420)
        size = app.document().content_size()
        print(f"laid out {size.width}x{size.height}")
        return

    app.launch(native=True)


if __name__ == "__main__":
    main()
