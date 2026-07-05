//! Headless integration tests (no window, no GPU). Runs everything in
//! one #[test] because AffineUI is single-threaded by contract and
//! cargo's default parallel test harness would violate it.

use affineui::{Document, Event, Theme, Validity, View};
use std::cell::Cell;
use std::rc::Rc;

#[test]
fn headless_contracts() {
    // Version + ABI gate.
    assert!(!affineui::version().is_empty());

    // Headless document: layout + content size + live mutation.
    let doc = Document::new();
    doc.set_html("<main><h1>Hello</h1><p id='msg'>Rust</p></main>");
    doc.set_user_stylesheet("main { padding: 16px; }", None);
    doc.layout(640, 480);
    let (w, h) = doc.content_size();
    assert!(w > 0 && h > 0, "content size {w}x{h}");
    assert!(doc.set_text_by_id("msg", "updated"));
    assert!(!doc.set_text_by_id("nope", "x"));
    let _ = doc.dispatch(&Event::mouse_move(10, 10));

    // View builders + widget handles.
    let view = View::new(Theme::Decius);
    view.build(|v| {
        v.heading(1, "Title", "", "title");
        v.button("Go", true, "go");
        v.container("row", "box", |v| {
            v.paragraph("inside", "", "para");
        });
        v.dropdown("Blend", &["Normal", "Multiply"], "Normal", "blend");
        v.slider("Exposure", 0.5, 0.0, 1.0, "exposure");
    });

    let title = view.find_widget("title");
    assert!(title.is_valid());
    assert_eq!(title.text(), "Title");

    title.set_attr("data-x", "42");
    assert_eq!(title.attr("data-x", ""), "42");
    assert!(title.has_attr("data-x"));

    // Degrade-don't-crash: missing widgets are inert, not errors.
    let missing = view.find_widget("does-not-exist");
    assert!(!missing.is_valid());
    assert_eq!(missing.text(), "");
    missing.set_text("ignored"); // must no-op

    // Typed components: Valid / WrongType / NotPresent.
    let blend = view.dropdown_at("blend");
    assert!(blend.is_valid());
    let wrong = view.checkbox_at("blend"); // dropdown queried as checkbox
    assert_eq!(wrong.validity(), Validity::WrongType);
    assert!(wrong.attached());
    assert!(!wrong.checked()); // typed read inert
    let absent = view.slider_at("nope");
    assert_eq!(absent.validity(), Validity::NotPresent);
    assert_eq!(view.slider_at("exposure").value(-1.0), 0.5);

    // HTML output includes the built widgets.
    let html = view.to_html_fragment();
    assert!(html.contains("Title") && html.contains("inside"), "html: {html}");

    // Closure lifetime: user_free must run exactly once, on view drop.
    struct DropFlag(Rc<Cell<u32>>);
    impl Drop for DropFlag {
        fn drop(&mut self) {
            self.0.set(self.0.get() + 1);
        }
    }
    let drops = Rc::new(Cell::new(0));
    {
        let view = View::new(Theme::Plain);
        view.build(|v| {
            let flag = DropFlag(Rc::clone(&drops));
            v.button("cb", false, "cb").on_click(move || {
                let _ = &flag; // owned by the closure
            });
        });
        assert_eq!(drops.get(), 0, "closure must live while the view does");
    }
    assert_eq!(drops.get(), 1, "closure must be released exactly once");
}
