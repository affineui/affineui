//! A programmatic component showcase (Decius theme): menubar, toolbar,
//! form widgets, tree, foldout, status bar — the gradio/imgui-style
//! builder driven from Rust.
//!
//!   cargo run --example component_gallery              # native window
//!   cargo run --example component_gallery -- --headless  # print HTML

use affineui::{App, Config, Theme, View};

/// The repo's `examples/` dir, where the framework CSS bundles live
/// (`frameworks/css/decius-css-*.bundle.min.css`). Located from this
/// crate's source dir so it works regardless of the cargo run CWD.
fn repo_examples_dir() -> String {
    let dir = std::path::Path::new(env!("CARGO_MANIFEST_DIR"))
        .join("..")
        .join("..")
        .join("..")
        .join("examples");
    if !dir.join("frameworks").join("css").exists() {
        eprintln!(
            "warning: framework assets not found at {} — widgets will render unstyled",
            dir.display()
        );
    }
    dir.to_string_lossy().into_owned()
}

fn build_gallery(v: &View) {
    v.menu_bar("menubar", |v| {
        v.menu_brand("Gallery", "cube", "brand");
        v.menu_button("File", "file-menu", |v| {
            v.menu_item("New", "file", "Ctrl+N", "new")
                .on_click(|| println!("File > New"));
            v.menu_item("Open…", "folder", "Ctrl+O", "open")
                .on_click(|| println!("File > Open"));
            v.menu_separator("");
            v.menu_item("Exit", "", "", "exit").on_click(|| println!("File > Exit"));
        });
        v.menu_button("Edit", "edit-menu", |v| {
            v.menu_item("Undo", "undo", "Ctrl+Z", "undo");
            v.menu_item("Redo", "redo", "Ctrl+Y", "redo");
        });
        v.menu_spacer("");
        v.menu_meta("untitled.aui", "docname");
    });

    v.toolbar("main-toolbar", |v| {
        v.icon_button("save", "save").on_click(|| println!("save"));
        v.icon_button("undo", "tb-undo");
        v.icon_button("redo", "tb-redo");
        v.toolbar_separator("");
        v.icon_button("play", "play");
    });

    v.container("gallery-body", "body", |v| {
        v.panel("controls", |v| {
            v.heading(2, "Form controls", "", "");
            v.button("Primary action", true, "primary")
                .on_click(|| println!("primary action"));
            v.checkbox("Bilinear filtering", true, "bilinear")
                .on_change(|value| println!("bilinear = {value}"));
            v.toggle("Live preview", false, "live");
            v.input("Name", "Untitled", "text", "name")
                .on_change(|value| println!("name = {value}"));
            v.dropdown("Blend mode", &["Normal", "Multiply", "Screen", "Overlay"], "Normal", "blend")
                .on_change(|value| println!("blend = {value}"));
            v.slider("Exposure", 0.5, 0.0, 1.0, "exposure")
                .on_change(|value| println!("exposure = {value}"));
            v.knob("Gain", 0.3, 0.0, 1.0, false, "gain");
            v.colorfield("Tint", "#4a90d9", "tint")
                .on_change(|value| println!("tint = {value}"));
        });

        v.foldout("Advanced", false, "advanced", |v| {
            v.combo("X", 0.0, 0.01, "pos-x");
            v.combo("Y", 0.0, 0.01, "pos-y");
            v.textarea("Notes", "", 4, "notes");
        });

        v.tree("scene", |v| {
            v.tree_row("Scene", false, 0, "root");
            v.tree_row("Camera", false, 1, "camera");
            v.tree_row("Mesh: hero", true, 1, "hero");
            v.tree_row("Material", false, 2, "mat");
        });
    });

    v.status_bar("status", |v| {
        v.text("Ready", "status-text");
    });
}

fn main() {
    let headless = std::env::args().any(|a| a == "--headless");

    let view = View::new(Theme::Decius);
    view.build(build_gallery);

    // Typed component queries (components.h semantics).
    let blend = view.dropdown_at("blend");
    println!("blend valid={} selected={:?}", blend.is_valid(), blend.selected());
    let exposure = view.slider_at("exposure");
    println!("exposure = {}", exposure.value(0.0));

    if headless {
        println!("{}", view.to_html_fragment());
        return;
    }

    let assets = repo_examples_dir();
    let app = App::new(
        Config::default()
            .title("AffineUI Rust — Component Gallery")
            .size(1100, 780)
            .asset_folders(&[assets.as_str(), "."]),
    );
    app.load_view(&view);
    std::process::exit(app.run());
}
