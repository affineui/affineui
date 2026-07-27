// Declarative docking from C# (issue #119).
//
// Before this, a dockable app could not be built in .NET at all — the C ABI the
// wrapper binds through had no docking surface. This asserts the EMITTED DOM,
// not just "it didn't throw": a binding that silently dropped every panel would
// pass the latter.
//
// A console app rather than a unit-test project, to match how the Hello example
// is exercised in CI (`dotnet run`), and because AffineUI is single-threaded by
// contract — a parallel test runner would violate it.
//
//     dotnet run --project bindings/csharp/tests/Dock

using AffineUI;

int failures = 0;

void Check(bool ok, string what)
{
    if (ok)
    {
        Console.WriteLine($"  ok    {what}");
    }
    else
    {
        Console.WriteLine($"  FAIL  {what}");
        failures++;
    }
}

Console.WriteLine("AffineUI .NET docking\n");

// ── a full workspace: document + panels, tabbed, floating, torn off ─────────
{
    int built = 0;
    var ids = new List<string>();

    using var view = new View(Theme.Decius);
    view.Build(v =>
    {
        v.DocumentView("workspace", v =>
        {
            // The center pane.
            string doc = v.Document("Scene", "cube", v =>
            {
                built++;
                v.Heading(1, "Viewport");
            });
            Check(doc.Length > 0, "Document() returns a pane id");

            // Its tab toolbar, by that id.
            v.DockToolbar(doc, v =>
            {
                built++;
                v.IconButton("cube", key: "tb-cube");
            });

            // Docked left, sized.
            string outliner = v.DockPanel(
                "Outliner", DockLocation.Docked(Dock.Left).Sized(280), "list", "outliner",
                v =>
                {
                    built++;
                    v.Heading(2, "Objects");
                });

            // Tabbed INTO the outliner, by the id it just handed back. That
            // round-trip is the whole reason the builders return an id.
            v.DockPanel(
                "Layers", DockLocation.AsTab().In(outliner), "", "layers",
                v =>
                {
                    built++;
                    v.Heading(2, "Layer list");
                });

            // Floating.
            string tools = v.DockPanel(
                "Tools", DockLocation.Floating(DockCorner.TopRight, (40, 60), (320, 240)), "", "tools",
                v =>
                {
                    built++;
                    v.Heading(2, "Toolbox");
                });

            // Torn off, dragging along with the floating panel.
            v.DockPanel(
                "Notes",
                DockLocation.TornOff(DockCorner.BottomLeft, (10, 10), (200, 160)).DraggingWith(tools),
                "", "notes",
                v =>
                {
                    built++;
                    v.Heading(2, "Notes body");
                });

            ids.AddRange(new[] { doc, outliner, tools });
        });
    });

    // 1 document + 1 toolbar + 3 panels = 5.
    //
    // NOT 6: "Layers" is tabbed BEHIND "Outliner", and an inactive tab's body is
    // emitted as an empty placeholder whose content is built only when the tab is
    // selected. Lazy tabs are the design, so the binding must not force-build
    // them — and this asserts it doesn't.
    Check(built == 5, $"every VISIBLE dock content callback ran exactly once (got {built}, want 5)");
    Check(ids.TrueForAll(id => id.Length > 0), "every pane gets an id");

    string html = view.ToHtmlFragment();
    Check(html.Contains("dcs-dock"), "the dock DOM is emitted");
    foreach (var expect in new[]
             {
                 "Outliner", "Layers", "Tools", "Notes",  // titles (all four get a tab)
                 "Objects", "Toolbox", "Notes body",      // bodies of the VISIBLE panels
                 "Viewport",                              // the document pane's content
                 "tb-cube",                               // the document pane's tab toolbar
             })
    {
        Check(html.Contains(expect), $"emitted DOM contains \"{expect}\"");
    }
    // "Layers" has a TAB but no BODY yet — inactive, so its content is deferred.
    Check(!html.Contains("Layer list"), "an inactive tab's body is NOT built");
}

// ── providers: a saved workspace beats the declared seed ───────────────────
{
    int sizeAsks = 0, placementAsks = 0;

    using var view = new View(Theme.Decius);
    view.SetDockSizeProvider(_ =>
    {
        sizeAsks++;
        return 400;   // a SAVED size, which must beat the 280 declared below
    });
    view.SetDockPlacementProvider(_ =>
    {
        placementAsks++;
        return null;  // null = "no override; use the declared DockLocation"
    });
    view.SetDockActiveTabProvider(_ => string.Empty);

    view.Build(v =>
    {
        v.DocumentView("ws", v =>
        {
            v.Document("Doc", "", _ => { });
            v.DockPanel("Outliner", DockLocation.Docked(Dock.Left).Sized(280), "", "outliner", _ => { });
        });
    });

    Check(sizeAsks > 0, "the size provider is consulted");
    Check(placementAsks > 0, "the placement provider is consulted");

    string html = view.ToHtmlFragment();
    Check(html.Contains("400px"), "the saved size (400) wins over the declared seed (280)");
    Check(!html.Contains("280px"), "the declared seed is overridden");
}

// ── the SAVE half: Document.DockPaneSizes() reads splitter sizes back out ──
// The block above proves RESTORE (feed sizes in via the provider). This proves
// SAVE — without it an app could restore a layout it was never able to persist.
// Needs a real App/Document because DockPaneSizes() reads the live flex-basis
// from the DOM (which is exactly why a ToHtmlFragment-only test never caught the
// gap).
{
    using var app = new App(new AppConfig { Title = "Dock Save", Width = 1280, Height = 820 });
    using var view = new View(Theme.Decius);
    view.Build(v =>
    {
        v.DocumentView("ws", v =>
        {
            v.Document("Scene", "cube", v => v.Heading(1, "Viewport"));
            v.DockPanel("Outliner", DockLocation.Docked(Dock.Left).Sized(280), "list", "outliner",
                        v => v.Heading(2, "Objects"));
            v.DockPanel("Inspector", DockLocation.Docked(Dock.Right).Sized(320), "sliders", "inspector",
                        v => v.Heading(2, "Properties"));
        });
    });
    app.LoadView(view);

    var sizes = app.Document.DockPaneSizes();
    Check(sizes.Count > 0, "DockPaneSizes() returns something — the SAVE half works");

    int? Find(string id) => sizes
        .Where(p => p.PaneId.Contains(id))
        .Select(p => (int?)p.Size)
        .FirstOrDefault();

    Check(Find("outliner") == 280, $"left pane size read back (got {Find("outliner")})");
    Check(Find("inspector") == 320, $"right pane size read back (got {Find("inspector")})");
    Check(Find("scene") is null && Find("cube") is null,
          "the flexible center pane has no fixed basis and is omitted");
}

// ── the deferred callbacks are released exactly once ───────────────────────
// The dock builders are DEFERRED: the engine holds their GCHandle past the call
// and frees it through user_free. A leak (never freed) or a double-free is a
// real crash, so count the releases the core reports.
{
    long before = AffineUIRuntime.ReleasedCallbackCount;
    {
        using var view = new View(Theme.Decius);
        view.SetDockSizeProvider(_ => 0);
        view.Build(v =>
        {
            v.DocumentView("ws", v =>
            {
                v.Document("Doc", "", _ => { });
                v.DockPanel("P", DockLocation.Docked(Dock.Left), "", "p", _ => { });
            });
        });
    }   // view disposed => the core drops its callbacks => user_free fires
    long released = AffineUIRuntime.ReleasedCallbackCount - before;
    // 1 size provider + 1 document content + 1 panel content = 3.
    Check(released >= 3, $"deferred callbacks are released on dispose (got {released}, want >= 3)");
}

Console.WriteLine();
if (failures > 0)
{
    Console.WriteLine($"FAILED: {failures} check(s)");
    return 1;
}
Console.WriteLine("all docking checks passed");
return 0;
