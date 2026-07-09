//! The README's "minimal complete program" — ships as a runnable example
//! so the first thing a new user types is also the thing we test.
//!
//!   cargo run --example hello                # opens a native window
//!   cargo run --example hello -- --headless  # prints HTML + layout size

use affineui::{App, Config, Theme, View};

fn main() {
    let view = View::new(Theme::Decius);
    view.build(|v| {
        v.heading(1, "Hello from Rust", "", "");
        v.paragraph("AffineUI — native HTML/CSS, no browser, no JS.", "", "");
        v.button("Click me", true, "go").on_click(|| println!("clicked!"));
        v.slider("Exposure", 0.5, 0.0, 1.0, "exposure")
            .on_change(|value| println!("exposure = {value}"));
        v.checkbox("Enabled", true, "enabled")
            .on_change(|value| println!("enabled = {value}"));
    });

    if std::env::args().any(|a| a == "--headless") {
        println!("{}", view.to_html_fragment());
        return;
    }

    // No asset_folders needed — the embedded Decius bundle supplies the
    // CSS, fonts, and icon font.
    let app = App::new(Config::default().title("AffineUI Rust").size(720, 480));
    app.load_view(&view);
    std::process::exit(app.run());
}
