//! Declarative docking from Rust (issue #119).
//!
//! Before this, a dockable app could not be built in Rust at all — the C ABI
//! Rust binds through had no docking surface. These tests build a real docking
//! view and assert the emitted DOM, because that is what a user sees: a test
//! that only checked "it didn't panic" would pass on a binding that silently
//! dropped every panel.
//!
//! One #[test], like headless.rs — AffineUI is single-threaded by contract and
//! cargo's parallel harness would violate it.

use affineui::{App, Config, Dock, DockCorner, DockLocation, Theme, View};
use std::cell::Cell;
use std::rc::Rc;

#[test]
fn docking() {
    // ── a full workspace: document + panels, tabbed, floating, torn off ──
    let built = Rc::new(Cell::new(0u32));

    let view = View::new(Theme::Decius);
    let ids = Rc::new(std::cell::RefCell::new(Vec::<String>::new()));

    {
        let built = built.clone();
        let ids = ids.clone();
        view.build(move |v| {
            v.document_view("workspace", |v| {
                // The center pane.
                let doc = v.document("Scene", "cube", {
                    let built = built.clone();
                    move |v| {
                        built.set(built.get() + 1);
                        v.heading(1, "Viewport", "", "");
                    }
                });
                assert!(!doc.is_empty(), "document() must return a pane id");

                // Its tab toolbar, by that id.
                v.dock_toolbar(&doc, {
                    let built = built.clone();
                    move |v| {
                        built.set(built.get() + 1);
                        v.icon_button("cube", "tb-cube");
                    }
                });

                // Docked left, sized.
                let outliner = v.dockpanel(
                    "Outliner",
                    DockLocation::docked(Dock::Left).sized(280),
                    "list",
                    "outliner",
                    {
                        let built = built.clone();
                        move |v| {
                            built.set(built.get() + 1);
                            v.heading(2, "Objects", "", "");
                        }
                    },
                );
                assert!(!outliner.is_empty());

                // Tabbed INTO the outliner — by the id it just handed back. That
                // round-trip is the whole reason dockpanel() returns an id.
                let layers = v.dockpanel(
                    "Layers",
                    DockLocation::tab().in_pane(&outliner),
                    "",
                    "layers",
                    {
                        let built = built.clone();
                        move |v| {
                            built.set(built.get() + 1);
                            v.heading(2, "Layer list", "", "");
                        }
                    },
                );

                // Floating.
                let tools = v.dockpanel(
                    "Tools",
                    DockLocation::floating(DockCorner::TopRight, (40, 60), (320, 240)),
                    "",
                    "tools",
                    {
                        let built = built.clone();
                        move |v| {
                            built.set(built.get() + 1);
                            v.heading(2, "Toolbox", "", "");
                        }
                    },
                );

                // Torn off, dragging along with the floating panel.
                v.dockpanel(
                    "Notes",
                    DockLocation::tearoff(DockCorner::BottomLeft, (10, 10), (200, 160))
                        .dragging_with(&tools),
                    "",
                    "notes",
                    {
                        let built = built.clone();
                        move |v| {
                            built.set(built.get() + 1);
                            v.heading(2, "Notes body", "", "");
                        }
                    },
                );

                ids.borrow_mut().extend([doc, outliner, layers, tools]);
            });
        });
    }

    // 1 document + 1 toolbar + 3 panels = 5.
    //
    // NOT 6: "Layers" is tabbed BEHIND "Outliner", and an inactive tab's body is
    // emitted as an empty placeholder whose content is built only when the tab is
    // selected (see View::set_dock_active_tab_provider). Lazy tabs are the design,
    // so the binding must not force-build them — and this asserts it doesn't.
    assert_eq!(built.get(), 5, "every VISIBLE dock content closure runs exactly once");

    let ids = ids.borrow();
    assert_eq!(ids.len(), 4);
    assert!(ids.iter().all(|id| !id.is_empty()), "every pane must get an id");

    let html = view.to_html_fragment();
    assert!(html.contains("dcs-dock"), "dock DOM must be emitted");
    for expect in [
        // Titles — every panel gets a tab, including the inactive one.
        "Outliner", "Layers", "Tools", "Notes",
        // Bodies of the VISIBLE panels.
        "Objects", "Toolbox", "Notes body",
        "Viewport", // the document pane's content
        "tb-cube",  // the document pane's tab toolbar
    ] {
        assert!(html.contains(expect), "missing {expect:?} in emitted dock DOM");
    }
    // "Layers" has a TAB but no BODY yet — it is inactive, so its content is
    // deferred until selected.
    assert!(!html.contains("Layer list"), "an inactive tab's body must not be built");

    // ── providers: a saved workspace beats the declared seed ──────────────
    let size_asks = Rc::new(Cell::new(0u32));
    let placement_asks = Rc::new(Cell::new(0u32));

    let view = View::new(Theme::Decius);
    {
        let size_asks = size_asks.clone();
        view.set_dock_size_provider(move |_pane_id| {
            size_asks.set(size_asks.get() + 1);
            400 // a SAVED size, which must beat the 280 declared below
        });
    }
    {
        let placement_asks = placement_asks.clone();
        // None = "no override; use the declared DockLocation".
        view.set_dock_placement_provider(move |_panel_id| {
            placement_asks.set(placement_asks.get() + 1);
            None
        });
    }
    view.set_dock_active_tab_provider(|_pane_id| String::new());

    view.build(|v| {
        v.document_view("ws", |v| {
            v.document("Doc", "", |_| {});
            v.dockpanel(
                "Outliner",
                DockLocation::docked(Dock::Left).sized(280),
                "",
                "outliner",
                |_| {},
            );
        });
    });

    assert!(size_asks.get() > 0, "the size provider must be consulted");
    assert!(placement_asks.get() > 0, "the placement provider must be consulted");

    let html = view.to_html_fragment();
    assert!(
        html.contains("400px"),
        "the saved size (400) must win over the declared seed (280); got: {html}"
    );
    assert!(!html.contains("280px"), "the declared seed must have been overridden");

    // ── the providers' closures are dropped exactly once ─────────────────
    // A leak or a double-free here is a real crash, not a test nicety.
    let drops = Rc::new(Cell::new(0u32));
    struct DropCount(Rc<Cell<u32>>);
    impl Drop for DropCount {
        fn drop(&mut self) {
            self.0.set(self.0.get() + 1);
        }
    }
    {
        let view = View::new(Theme::Decius);
        let guard = DropCount(drops.clone());
        view.set_dock_size_provider(move |_| {
            let _ = &guard; // move the guard into the closure
            0
        });
        assert_eq!(drops.get(), 0, "still alive while the view is");
    }
    assert_eq!(drops.get(), 1, "provider closure must be dropped exactly once");
}

/// The SAVE half of size persistence: `Document::dock_pane_sizes`.
///
/// This was the gap. Rust could `set_dock_size_provider` (feed sizes back IN)
/// but had no way to read them back OUT — so an app could restore a layout it
/// was never able to save, and a user's splitter drags were unpersistable.
/// `dock_overrides()` doesn't cover it: that records only *structure* (where a
/// panel was dragged/torn off to), not pane size.
///
/// Materializes the view into a real Document, because dock_pane_sizes() reads
/// the live flex-basis from the DOM — a View-only test cannot see it, which is
/// exactly why the gap went unnoticed.
#[test]
fn dock_pane_sizes_round_trip() {
    let app = App::new(Config::default());

    let view = View::new(Theme::Decius);
    view.build(|v| {
        v.document_view("workspace", |v| {
            v.document("Scene", "cube", |v| {
                v.heading(1, "Viewport", "", "");
            });
            v.dockpanel(
                "Outliner",
                DockLocation::docked(Dock::Left).sized(280),
                "list",
                "outliner",
                |v| {
                    v.heading(2, "Objects", "", "");
                },
            );
            v.dockpanel(
                "Inspector",
                DockLocation::docked(Dock::Right).sized(320),
                "sliders",
                "inspector",
                |v| {
                    v.heading(2, "Properties", "", "");
                },
            );
        });
    });
    app.load_view(&view);

    let sizes = app.document().dock_pane_sizes();
    assert!(
        !sizes.is_empty(),
        "dock_pane_sizes() returned nothing — the SAVE half of persistence is broken"
    );

    let find = |id: &str| -> Option<i32> {
        sizes
            .iter()
            .find(|(pane, _)| pane.contains(id))
            .map(|(_, px)| *px)
    };

    // The declared flex-basis must come back out, per pane.
    assert_eq!(find("outliner"), Some(280), "left pane size, got {sizes:?}");
    assert_eq!(find("inspector"), Some(320), "right pane size, got {sizes:?}");

    // The flexible center/document pane has no fixed basis and is omitted.
    assert!(
        find("scene").is_none() && find("cube").is_none(),
        "the flexible center pane must NOT appear: {sizes:?}"
    );

    // The full loop: persist `sizes`, then restore them into a FRESH workspace
    // and prove they actually applied. The restore panels are declared with a
    // WRONG seed (100) on purpose — reading back 280/320 can only mean the saved
    // values won through the provider, not that the seed happened to match.
    let saved: Vec<(String, i32)> = sizes.clone();

    let restore_app = App::new(Config::default());
    let restore = View::new(Theme::Decius);
    restore.set_dock_size_provider(move |pane_id| {
        saved.iter().find(|(id, _)| id == pane_id).map(|(_, px)| *px).unwrap_or(0)
    });
    restore.build(|v| {
        v.document_view("workspace", |v| {
            v.document("Scene", "cube", |v| {
                v.heading(1, "Viewport", "", "");
            });
            v.dockpanel(
                "Outliner",
                DockLocation::docked(Dock::Left).sized(100), // wrong seed
                "list",
                "outliner",
                |v| {
                    v.heading(2, "Objects", "", "");
                },
            );
            v.dockpanel(
                "Inspector",
                DockLocation::docked(Dock::Right).sized(100), // wrong seed
                "sliders",
                "inspector",
                |v| {
                    v.heading(2, "Properties", "", "");
                },
            );
        });
    });
    restore_app.load_view(&restore);

    let restored = restore_app.document().dock_pane_sizes();
    let rfind = |id: &str| -> Option<i32> {
        restored.iter().find(|(pane, _)| pane.contains(id)).map(|(_, px)| *px)
    };
    assert_eq!(rfind("outliner"), Some(280), "saved left size restored, got {restored:?}");
    assert_eq!(rfind("inspector"), Some(320), "saved right size restored, got {restored:?}");
}
