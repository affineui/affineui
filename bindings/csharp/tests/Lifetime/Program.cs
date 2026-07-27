using AffineUI;

static void Check(bool condition, string message)
{
    if (!condition)
        throw new InvalidOperationException(message);
}

// Immediate scope-builder callback.
View? escaped = null;
using (var owner = new View(Theme.Plain))
{
    owner.Build(v =>
    {
        v.Panel("weak-panel", callbackView => escaped = callbackView);
    });
    Check(escaped is { IsAlive: true }, "callback View should resolve while its owner is alive");
}

Check(escaped is { IsAlive: false }, "escaped callback View should invalidate on owner disposal");
try
{
    escaped!.Heading(1, "ignored", key: "ignored");
    throw new InvalidOperationException("stale View use did not throw");
}
catch (ObjectDisposedException)
{
    // A managed exception is the deliberate stale-reference failure mode.
}
escaped!.Dispose();

// Persistent App.SetView callback: a distinct trampoline and native-owned View.
View? appEscaped = null;
using (var app = new App(new AppConfig { Title = "weak callback lifetime" }))
{
    app.SetView(v => appEscaped = v);
    Check(appEscaped is { IsAlive: true }, "App callback View should resolve while App owns it");
}

Check(appEscaped is { IsAlive: false }, "App callback View should invalidate on App disposal");
try
{
    appEscaped!.Clear();
    throw new InvalidOperationException("stale App callback View use did not throw");
}
catch (ObjectDisposedException)
{
}
appEscaped!.Dispose();

Console.WriteLine("callback View weak-lifetime checks passed");
