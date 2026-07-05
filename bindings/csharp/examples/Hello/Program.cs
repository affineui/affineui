// Hello — the C# counterpart of bindings/python/examples/hello.py.
//
//   dotnet run -- --headless   builds a View programmatically, prints its HTML
//                              fragment, lays it out in a headless Document,
//                              and verifies the callback-lifetime contract.
//   dotnet run                 additionally opens a native window and runs the
//                              app loop (windowed verification is a human
//                              step; CI uses --headless).

using AffineUI;

internal static class Program
{
    /// <summary>
    /// The repo's examples/ dir, where the framework CSS bundles live
    /// (frameworks/css/decius-css-*.bundle.min.css). Located by walking up
    /// from the exe so the run CWD doesn't matter; null outside a checkout.
    /// </summary>
    private static string? FindRepoExamplesDir()
    {
        for (DirectoryInfo? dir = new(AppContext.BaseDirectory); dir is not null; dir = dir.Parent)
        {
            string candidate = Path.Combine(dir.FullName, "examples", "frameworks", "css");
            if (Directory.Exists(candidate))
                return Path.Combine(dir.FullName, "examples");
        }
        return null;
    }

    private static int Main(string[] args)
    {
        bool headless = Array.Exists(args, static a => a == "--headless");

        // First native touch: loads affineui_c and gates on the C ABI version.
        Console.WriteLine($"AffineUI {AffineUIRuntime.Version} (C ABI v{AffineUIRuntime.AbiVersion})");

        using var view = new View(Theme.Decius);
        view.Build(v =>
        {
            v.Heading(1, "Hello from C#");
            v.Paragraph("AffineUI is alive.", key: "message");
            v.Button("Click me", primary: true, key: "go")
                .OnClick(static () => Console.WriteLine("button clicked"));
            v.Slider("Exposure", 0.5, 0.0, 1.0, key: "exposure")
                .OnChange(static value => Console.WriteLine($"exposure = {value}"));
            v.Checkbox("Enabled", isChecked: true, key: "enabled")
                .OnChange(static value => Console.WriteLine($"enabled = {value}"));
        });

        // Typed components (components.h semantics) over the live tree.
        Checkbox enabled = view.CheckboxAt("enabled");
        Slider exposure = view.SliderAt("exposure");
        Button missing = view.ButtonAt("does-not-exist");
        Console.WriteLine($"checkbox 'enabled':  validity={enabled.Validity} checked={enabled.Checked}");
        Console.WriteLine($"slider 'exposure':   validity={exposure.Validity} value={exposure.Value}");
        Console.WriteLine($"button 'missing':    validity={missing.Validity} (inert, safe)");

        if (!headless)
        {
            string? assets = FindRepoExamplesDir();
            if (assets is null)
                Console.Error.WriteLine("warning: framework assets not found — widgets will render unstyled");
            using var app = new App(new AppConfig
            {
                Title = "AffineUI C# Hello",
                Width = 960,
                Height = 640,
                AssetFolders = assets is null ? null : new[] { assets, "." },
            });
            app.LoadView(view);
            return app.Run();
        }

        // ── Headless verification path ───────────────────────────────────

        string fragment = view.ToHtmlFragment();
        Console.WriteLine("--- html fragment ---");
        Console.WriteLine(fragment.Trim());
        Console.WriteLine("---------------------");

        using (var doc = new Document())
        {
            doc.SetHtml(view.ToHtmlDocument());
            doc.Layout(640, 360);
            Size size = doc.ContentSize;
            Console.WriteLine($"headless layout content size: {size.Width} x {size.Height}");
            if (size.Width <= 0 || size.Height <= 0)
            {
                Console.Error.WriteLine("FAIL: headless layout returned a non-positive content size.");
                return 1;
            }
        }

        // Callback lifetime: the core must release each registered handler
        // (user_free → GCHandle.Free) exactly once when its view is destroyed.
        long before = AffineUIRuntime.ReleasedCallbackCount;
        using (var scratch = new View(Theme.Plain))
        {
            scratch.Build(static v =>
            {
                v.Button("temp", key: "temp-button").OnClick(static () => { });
                v.Checkbox("temp", key: "temp-check").OnChange(static _ => { });
            });
        }
        long released = AffineUIRuntime.ReleasedCallbackCount - before;
        Console.WriteLine($"callback holders released on view dispose: {released}");
        if (released != 2)
        {
            Console.Error.WriteLine($"FAIL: expected 2 released callback holders, got {released}.");
            return 1;
        }

        Console.WriteLine("OK");
        return 0;
    }
}
