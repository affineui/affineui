//! Headless integration tests (no window, no GPU). Runs everything in
//! one #[test] because AffineUI is single-threaded by contract and
//! cargo's default parallel test harness would violate it.

use affineui::{App, Config, Document, Event, Theme, Validity, View};
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
    let dispatch = doc.dispatch(&Event::mouse_move(10, 10));
    assert!(!dispatch.event_consumed);
    doc.set_caret_blink_interval(0.0);
    assert_eq!(doc.caret_blink_interval(), 0.0);
    assert!(!doc.tick_caret_blink());

    // Capture phase crosses the C ABI in both directions and consumes before
    // the document/widget phase. Event data must survive the callback copy.
    let capture_calls = Rc::new(Cell::new(0));
    let capture_drops = Rc::new(Cell::new(0));
    struct CaptureDrop(Rc<Cell<u32>>);
    impl Drop for CaptureDrop {
        fn drop(&mut self) {
            self.0.set(self.0.get() + 1);
        }
    }
    {
        let app = App::new(Config::default());
        let calls = Rc::clone(&capture_calls);
        let drop_flag = CaptureDrop(Rc::clone(&capture_drops));
        app.on_event_capture(move |event| {
            let _ = &drop_flag;
            assert_eq!(event.kind, affineui::EventType::MouseMove);
            assert_eq!((event.x, event.y), (12, 34));
            calls.set(calls.get() + 1);
            true
        });
        assert!(app.dispatch(&Event::mouse_move(12, 34)));
    }
    assert_eq!(capture_calls.get(), 1);
    assert_eq!(capture_drops.get(), 1, "capture closure released on app drop");

    let embedded_calls = Rc::new(Cell::new(0));
    {
        let ui = affineui::embedded::Ui::new();
        ui.set_caret_blink_interval(0.0);
        assert_eq!(ui.caret_blink_interval(), 0.0);
        let calls = Rc::clone(&embedded_calls);
        ui.on_event_capture(move |event| {
            assert_eq!(event.kind, affineui::EventType::MouseDown);
            calls.set(calls.get() + 1);
            true
        });
        let mut event = Event::mouse_move(3, 4);
        event.kind = affineui::EventType::MouseDown;
        assert!(ui.dispatch(&event));
    }
    assert_eq!(embedded_calls.get(), 1);

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
