// View — the gradio/imgui-style component-tree builder (Mode A + shared).
// Wraps affineui_view_*. Builder methods append widgets at the current
// insertion point and return caller-owned Widget handles; scope builders take
// an Action<View> invoked synchronously while the scope is open.

using System.Runtime.InteropServices;

namespace AffineUI;

/// <summary>
/// A retained widget command tree, reconciled into a document when loaded
/// into an <see cref="App"/>. Build with <see cref="Build"/> (or
/// <see cref="Begin"/>/<see cref="End"/> directly); rebuild any time — the
/// tree is diffed, and <see cref="Widget"/> handles re-find their node by key.
/// </summary>
public sealed partial class View : IDisposable
{
    internal sealed class ViewSafeHandle : SafeHandle
    {
        public ViewSafeHandle(IntPtr h) : base(IntPtr.Zero, ownsHandle: true)
        {
            SetHandle(h);
        }

        public override bool IsInvalid => handle == IntPtr.Zero;

        protected override bool ReleaseHandle()
        {
            NativeMethods.affineui_view_destroy(handle);
            return true;
        }
    }

    internal sealed class WeakViewSafeHandle : SafeHandle
    {
        public WeakViewSafeHandle(IntPtr h) : base(IntPtr.Zero, ownsHandle: true)
        {
            SetHandle(h);
        }

        public override bool IsInvalid => handle == IntPtr.Zero;

        protected override bool ReleaseHandle()
        {
            NativeMethods.affineui_weak_view_destroy(handle);
            return true;
        }
    }

    private readonly ViewSafeHandle? _ownedHandle;
    private readonly WeakViewSafeHandle? _weakHandle;

    /// <summary>Creates a new view. Default theme is <see cref="Theme.Decius"/>
    /// — the framework the compile-time bundle ships CSS + fonts for, so a
    /// bare <c>new View()</c> produces styled widgets out of the box.</summary>
    public View(Theme theme = Theme.Decius)
    {
        AffineUIRuntime.EnsureLoaded();
        AffineUIRuntime.CheckThread();
        IntPtr h = NativeMethods.affineui_view_create((int)theme);
        if (h == IntPtr.Zero)
            throw new InvalidOperationException("affineui_view_create failed.");
        _ownedHandle = new ViewSafeHandle(h);
    }

    private View(IntPtr borrowed)
    {
        IntPtr weak = NativeMethods.affineui_view_weak_ref(borrowed);
        if (weak == IntPtr.Zero)
            throw new InvalidOperationException("affineui_view_weak_ref failed.");
        _weakHandle = new WeakViewSafeHandle(weak);
    }

    /// <summary>Wraps a native callback View with its invalidating weak-lifetime
    /// token. The managed wrapper may safely escape the callback; it becomes
    /// invalid after the native View is destroyed and subsequent operations
    /// throw <see cref="ObjectDisposedException"/>.</summary>
    internal static View Borrowed(IntPtr handle) => new(handle);

    private IntPtr ResolvedHandle
    {
        get
        {
            if (_ownedHandle is { IsClosed: false, IsInvalid: false } owned)
                return owned.DangerousGetHandle();

            if (_weakHandle is not { IsClosed: false, IsInvalid: false } weak)
                return IntPtr.Zero;

            IntPtr resolved = NativeMethods.affineui_weak_view_get(weak.DangerousGetHandle());
            GC.KeepAlive(weak);
            return resolved;
        }
    }

    /// <summary>Whether the native View is still alive. Callback Views become
    /// false after their framework owner is destroyed.</summary>
    public bool IsAlive => ResolvedHandle != IntPtr.Zero;

    internal IntPtr Handle
    {
        get
        {
            IntPtr resolved = ResolvedHandle;
            if (resolved == IntPtr.Zero)
                throw new ObjectDisposedException(
                    nameof(View),
                    "The native AffineUI View that supplied this callback wrapper no longer exists.");
            return resolved;
        }
    }

    /// <summary>
    /// Destroys the native view. Existing Widget handles weakly invalidate.
    /// Further operations on this View throw <see cref="ObjectDisposedException"/>.
    /// </summary>
    public void Dispose()
    {
        _ownedHandle?.Dispose();
        _weakHandle?.Dispose();
    }

    internal Widget Wrap(IntPtr widgetHandle)
    {
        return new Widget(widgetHandle);
    }

    // ── Lifecycle / reconciliation ───────────────────────────────────────

    /// <summary>Removes every widget from the tree.</summary>
    public void Clear()
    {
        NativeMethods.affineui_view_clear(Handle);
        GC.KeepAlive(this);
    }

    /// <summary>Starts a reconcile pass (imgui-style: re-declare the tree, the
    /// diff is applied on <see cref="End"/>).</summary>
    public void Begin()
    {
        AffineUIRuntime.CheckThread();
        NativeMethods.affineui_view_begin(Handle);
        GC.KeepAlive(this);
    }

    /// <summary>Ends the reconcile pass started by <see cref="Begin"/>.</summary>
    public void End()
    {
        NativeMethods.affineui_view_end(Handle);
        GC.KeepAlive(this);
    }

    /// <summary>Runs one full reconcile pass: <see cref="Begin"/>, your
    /// builder, <see cref="End"/>.</summary>
    public void Build(Action<View> build)
    {
        ArgumentNullException.ThrowIfNull(build);
        Begin();
        try { build(this); }
        finally { End(); }
    }

    /// <summary>The CSS framework theme this view targets.</summary>
    public Theme Theme
    {
        get
        {
            var t = (Theme)NativeMethods.affineui_view_get_theme(Handle);
            GC.KeepAlive(this);
            return t;
        }
        set
        {
            NativeMethods.affineui_view_set_theme(Handle, (int)value);
            GC.KeepAlive(this);
        }
    }

    /// <summary>Declares the CSS framework version this view targets
    /// (e.g. "0.5.2" for Decius). Empty string restores the default.</summary>
    public void SetFrameworkVersion(string version)
    {
        NativeMethods.affineui_view_set_framework_version(Handle, version);
        GC.KeepAlive(this);
    }

    /// <summary>Sets a selector attribute on the view root.</summary>
    public void Selector(string name, string value)
    {
        NativeMethods.affineui_view_selector(Handle, name, value);
        GC.KeepAlive(this);
    }

    /// <summary>Serializes the widget tree to an HTML fragment.</summary>
    public string ToHtmlFragment()
    {
        var s = AffineUIRuntime.TakeString(NativeMethods.affineui_view_to_html_fragment(Handle));
        GC.KeepAlive(this);
        return s;
    }

    /// <summary>Serializes the widget tree to a complete HTML document
    /// (including the theme's framework stylesheet reference).</summary>
    public string ToHtmlDocument()
    {
        var s = AffineUIRuntime.TakeString(NativeMethods.affineui_view_to_html_document(Handle));
        GC.KeepAlive(this);
        return s;
    }

    /// <summary>Looks up a widget by its stable key. Always returns a handle;
    /// check <see cref="Widget.IsValid"/>.</summary>
    public Widget FindWidget(string name)
    {
        var w = Wrap(NativeMethods.affineui_view_find_widget(Handle, name));
        GC.KeepAlive(this);
        return w;
    }

    // ── Text / content widgets ───────────────────────────────────────────

    public Widget Heading(int level, string text, string classes = "", string key = "")
    {
        var w = Wrap(NativeMethods.affineui_view_heading(Handle, level, text, classes, key));
        GC.KeepAlive(this);
        return w;
    }

    public Widget Paragraph(string text, string classes = "", string key = "")
    {
        var w = Wrap(NativeMethods.affineui_view_paragraph(Handle, text, classes, key));
        GC.KeepAlive(this);
        return w;
    }

    public Widget Text(string text, string key = "")
    {
        var w = Wrap(NativeMethods.affineui_view_text(Handle, text, key));
        GC.KeepAlive(this);
        return w;
    }

    /// <summary>Trusted raw HTML, parsed when the view is loaded into a
    /// document.</summary>
    public Widget Html(string markup, string key = "")
    {
        var w = Wrap(NativeMethods.affineui_view_html(Handle, markup, key));
        GC.KeepAlive(this);
        return w;
    }

    // ── Form widgets ─────────────────────────────────────────────────────

    public Widget Button(string label, bool primary = false, string key = "")
    {
        var w = Wrap(NativeMethods.affineui_view_button(Handle, label, primary ? 1 : 0, key));
        GC.KeepAlive(this);
        return w;
    }

    public Widget Checkbox(string label, bool isChecked = false, string key = "")
    {
        var w = Wrap(NativeMethods.affineui_view_checkbox(Handle, label, isChecked ? 1 : 0, key));
        GC.KeepAlive(this);
        return w;
    }

    public Widget Toggle(string label, bool on = false, string key = "")
    {
        var w = Wrap(NativeMethods.affineui_view_toggle(Handle, label, on ? 1 : 0, key));
        GC.KeepAlive(this);
        return w;
    }

    public Widget Input(string label, string value = "", string type = "text", string key = "")
    {
        var w = Wrap(NativeMethods.affineui_view_input(Handle, label, value, type, key));
        GC.KeepAlive(this);
        return w;
    }

    public Widget Password(string label, string value = "", string key = "")
    {
        var w = Wrap(NativeMethods.affineui_view_password(Handle, label, value, key));
        GC.KeepAlive(this);
        return w;
    }

    public Widget TextArea(string label, string value = "", int rows = 3, string key = "")
    {
        var w = Wrap(NativeMethods.affineui_view_textarea(Handle, label, value, rows, key));
        GC.KeepAlive(this);
        return w;
    }

    public Widget Dropdown(string label, IReadOnlyList<string> options, string selected = "", string key = "")
    {
        using var opts = new Utf8StringArray(options);
        var w = Wrap(NativeMethods.affineui_view_dropdown(Handle, label, opts.Pointer, opts.Count, selected, key));
        GC.KeepAlive(this);
        return w;
    }

    public Widget ButtonGroup(string label, IReadOnlyList<string> options, string selected = "", string key = "")
    {
        using var opts = new Utf8StringArray(options);
        var w = Wrap(NativeMethods.affineui_view_button_group(Handle, label, opts.Pointer, opts.Count, selected, key));
        GC.KeepAlive(this);
        return w;
    }

    public Widget Slider(string label, double value, double min = 0.0, double max = 1.0, string key = "")
    {
        var w = Wrap(NativeMethods.affineui_view_slider(Handle, label, value, min, max, key));
        GC.KeepAlive(this);
        return w;
    }

    public Widget Knob(string label, double value, double min = 0.0, double max = 1.0,
                       bool bipolar = false, string key = "")
    {
        var w = Wrap(NativeMethods.affineui_view_knob(Handle, label, value, min, max, bipolar ? 1 : 0, key));
        GC.KeepAlive(this);
        return w;
    }

    /// <summary>Bare drag-scrub numeric combo (no field/label wrapper).
    /// <paramref name="linear"/> scrubs at a constant step/pixel (rotation
    /// degrees etc.); the default accelerates with the value's magnitude.
    /// </summary>
    public Widget Combo(string label, double value, double step = 1.0, string key = "",
                        bool linear = false)
    {
        var w = Wrap(NativeMethods.affineui_view_combo(Handle, label, value, step, key,
                                                       linear ? 1 : 0));
        GC.KeepAlive(this);
        return w;
    }

    /// <summary>Decius chip + hex + picker-popover color field.</summary>
    public Widget ColorField(string label, string value, string key = "")
    {
        var w = Wrap(NativeMethods.affineui_view_colorfield(Handle, label, value, key));
        GC.KeepAlive(this);
        return w;
    }

    /// <summary>Swatch-popup color field.</summary>
    public Widget ColorField(string label, string value, IReadOnlyList<string> swatches, string key = "")
    {
        using var sw = new Utf8StringArray(swatches);
        var w = Wrap(NativeMethods.affineui_view_color_field(Handle, label, value, sw.Pointer, sw.Count, key));
        GC.KeepAlive(this);
        return w;
    }

    // ── Scope builders (build runs synchronously) ────────────────────────

    public Widget Container(string classes = "", string key = "", Action<View>? build = null)
    {
        using var scope = BuildScope.Create(build);
        var w = Wrap(NativeMethods.affineui_view_container(Handle, classes, key, scope.Fn, scope.User));
        GC.KeepAlive(this);
        return w;
    }

    public Widget Element(string tag, string classes = "", string key = "", Action<View>? build = null)
    {
        using var scope = BuildScope.Create(build);
        var w = Wrap(NativeMethods.affineui_view_element(Handle, tag, classes, key, scope.Fn, scope.User));
        GC.KeepAlive(this);
        return w;
    }

    public Widget Panel(string key = "", Action<View>? build = null)
    {
        using var scope = BuildScope.Create(build);
        var w = Wrap(NativeMethods.affineui_view_panel(Handle, key, scope.Fn, scope.User));
        GC.KeepAlive(this);
        return w;
    }

    // ── Declarative docking ──────────────────────────────────────────
    //
    // NOTE the deliberate absence of BuildScope below. The three dock builders
    // are DEFERRED: the engine records the content callback and invokes it later,
    // when the container resolves and emits the layout. BuildScope frees its
    // GCHandle when the native call RETURNS ("NOT retained", per its own doc), so
    // using it here would hand the engine a freed handle — a use-after-free that
    // C# would not catch. Instead the GCHandle is allocated un-disposed and the
    // engine's user_free releases it, exactly once.

    /// <summary>
    /// Declare a dock container. Inside <paramref name="build"/>, call
    /// <see cref="Document"/> for the center pane and <see cref="DockPanel"/> for
    /// the panels around it.
    /// </summary>
    public Widget DocumentView(string key, Action<View> build)
    {
        using var scope = BuildScope.Create(build);   // immediate — runs before return
        var w = Wrap(NativeMethods.affineui_view_document_view(Handle, key, scope.Fn, scope.User));
        GC.KeepAlive(this);
        return w;
    }

    /// <summary>
    /// The center/document pane. Only valid inside a <see cref="DocumentView"/> build.
    /// Returns the pane id — pass it to <see cref="DockLocation.In"/> to parent a
    /// panel to it, or to <see cref="DockToolbar"/> to give it a tab toolbar.
    /// </summary>
    /// <param name="icon">A Decius icon-font glyph name ("" for none).</param>
    public string Document(string title, string icon, Action<View> content)
    {
        var (fn, user, free) = Deferred(content);
        var id = AffineUIRuntime.TakeString(
            NativeMethods.affineui_view_document(Handle, fn, user, free, title, icon));
        GC.KeepAlive(this);
        return id;
    }

    /// <summary>
    /// Declare a dockable panel. Only valid inside a <see cref="DocumentView"/> build.
    /// Returns the panel id, usable as another panel's parent and as the pane id
    /// the dock providers are asked about.
    /// </summary>
    public string DockPanel(string title, DockLocation where, string icon, string key, Action<View> content)
    {
        var (fn, user, free) = Deferred(content);
        using var loc = where.ToNative();
        var raw = loc.Raw;
        var id = AffineUIRuntime.TakeString(
            NativeMethods.affineui_view_dockpanel(Handle, title, ref raw, fn, user, free, icon, key));
        GC.KeepAlive(this);
        return id;
    }

    /// <summary>
    /// Give a dock pane its tab toolbar — the strip beside the tabs (filter
    /// buttons, a search field, a viewport's mode/tool controls).
    /// </summary>
    /// <param name="paneId">What <see cref="Document"/> or <see cref="DockPanel"/> returned.</param>
    public void DockToolbar(string paneId, Action<View> build)
    {
        var (fn, user, free) = Deferred(build);
        NativeMethods.affineui_view_dock_toolbar(Handle, paneId, fn, user, free);
        GC.KeepAlive(this);
    }

    /// <summary>
    /// A deferred build callback: the GCHandle is NOT disposed here — the engine
    /// releases it through user_free, exactly once, when it drops the callback.
    /// </summary>
    private static (IntPtr Fn, IntPtr User, IntPtr Free) Deferred(Action<View> build)
    {
        ArgumentNullException.ThrowIfNull(build);
        var handle = GCHandle.Alloc(build);
        return (Trampolines.Build, GCHandle.ToIntPtr(handle), Trampolines.FreeUser);
    }

    // ── Dock providers: a saved workspace beats the declared seed ────

    /// <summary>Supply the saved px size of each pane (return &lt;= 0 to fall back
    /// to the size declared in its <see cref="DockLocation"/>).</summary>
    public void SetDockSizeProvider(Func<string, int> provider)
    {
        ArgumentNullException.ThrowIfNull(provider);
        var handle = GCHandle.Alloc(provider);
        NativeMethods.affineui_view_set_dock_size_provider(
            Handle, Trampolines.DockSize, GCHandle.ToIntPtr(handle), Trampolines.FreeUser);
        GC.KeepAlive(this);
    }

    /// <summary>Supply the active tab of each dock leaf (empty selects the primary panel).</summary>
    public void SetDockActiveTabProvider(Func<string, string> provider)
    {
        ArgumentNullException.ThrowIfNull(provider);
        var state = new Trampolines.DockTabState { Fn = provider };
        var handle = GCHandle.Alloc(state);
        NativeMethods.affineui_view_set_dock_active_tab_provider(
            Handle, Trampolines.DockActiveTab, GCHandle.ToIntPtr(handle), Trampolines.FreeUser);
        GC.KeepAlive(this);
    }

    /// <summary>
    /// Supply the saved placement of each panel — where the user dragged or tore
    /// it to. Return null for "no override; use the declared
    /// <see cref="DockLocation"/>". Read the values back out with
    /// <c>Document.DockOverrides()</c>.
    /// </summary>
    public void SetDockPlacementProvider(Func<string, DockPlacement?> provider)
    {
        ArgumentNullException.ThrowIfNull(provider);
        var state = new Trampolines.DockPlacementState { Fn = provider };
        var handle = GCHandle.Alloc(state);
        NativeMethods.affineui_view_set_dock_placement_provider(
            Handle, Trampolines.DockPlacement, GCHandle.ToIntPtr(handle), Trampolines.FreeUser);
        GC.KeepAlive(this);
    }

    /// <summary>
    /// Replay the CURRENT arrangement — splits, tab order, active tabs, floats —
    /// straight from <paramref name="doc"/>, instead of the declared seed. This is
    /// what makes drag-to-dock and tearoff survive a view rebuild.
    /// </summary>
    public void SetDockLayoutFromDocument(Document doc)
    {
        ArgumentNullException.ThrowIfNull(doc);
        NativeMethods.affineui_view_set_dock_layout_from_document(Handle, doc.Handle);
        GC.KeepAlive(this);
        GC.KeepAlive(doc);
    }

    public Widget Card(string title, string classes = "", string key = "", Action<View>? build = null)
    {
        using var scope = BuildScope.Create(build);
        var w = Wrap(NativeMethods.affineui_view_card(Handle, title, classes, key, scope.Fn, scope.User));
        GC.KeepAlive(this);
        return w;
    }

    public Widget Toolbar(string key = "", Action<View>? build = null)
    {
        using var scope = BuildScope.Create(build);
        var w = Wrap(NativeMethods.affineui_view_toolbar(Handle, key, scope.Fn, scope.User));
        GC.KeepAlive(this);
        return w;
    }

    public Widget MenuBar(string key = "", Action<View>? build = null)
    {
        using var scope = BuildScope.Create(build);
        var w = Wrap(NativeMethods.affineui_view_menu_bar(Handle, key, scope.Fn, scope.User));
        GC.KeepAlive(this);
        return w;
    }

    public Widget StatusBar(string key = "", Action<View>? build = null)
    {
        using var scope = BuildScope.Create(build);
        var w = Wrap(NativeMethods.affineui_view_status_bar(Handle, key, scope.Fn, scope.User));
        GC.KeepAlive(this);
        return w;
    }

    public Widget Tree(string key = "", Action<View>? build = null)
    {
        using var scope = BuildScope.Create(build);
        var w = Wrap(NativeMethods.affineui_view_tree(Handle, key, scope.Fn, scope.User));
        GC.KeepAlive(this);
        return w;
    }

    public Widget Foldout(string title, bool expanded = true, string key = "", Action<View>? build = null)
    {
        using var scope = BuildScope.Create(build);
        var w = Wrap(NativeMethods.affineui_view_foldout(Handle, title, expanded ? 1 : 0, key, scope.Fn, scope.User));
        GC.KeepAlive(this);
        return w;
    }

    // ── Structural leaves ────────────────────────────────────────────────

    public Widget ToolbarSeparator(string key = "")
    {
        var w = Wrap(NativeMethods.affineui_view_toolbar_separator(Handle, key));
        GC.KeepAlive(this);
        return w;
    }

    public Widget IconButton(string icon, string key = "")
    {
        var w = Wrap(NativeMethods.affineui_view_icon_button(Handle, icon, key));
        GC.KeepAlive(this);
        return w;
    }

    /// <summary>Menubar button that owns its dropdown; fill the menu with
    /// <see cref="MenuItem"/> / <see cref="MenuSeparator"/> /
    /// <see cref="Submenu"/> inside <paramref name="build"/>.</summary>
    public Widget MenuButton(string label, Action<View>? build = null, string key = "")
    {
        using var scope = BuildScope.Create(build);
        var w = Wrap(NativeMethods.affineui_view_menu_button(Handle, label, scope.Fn, scope.User, key));
        GC.KeepAlive(this);
        return w;
    }

    public Widget MenuItem(string label, string icon = "", string shortcut = "", string key = "")
    {
        var w = Wrap(NativeMethods.affineui_view_menu_item(Handle, label, icon, shortcut, key));
        GC.KeepAlive(this);
        return w;
    }

    public Widget MenuSeparator(string key = "")
    {
        var w = Wrap(NativeMethods.affineui_view_menu_separator(Handle, key));
        GC.KeepAlive(this);
        return w;
    }

    public Widget Submenu(string label, Action<View>? build = null, string icon = "", string key = "")
    {
        using var scope = BuildScope.Create(build);
        var w = Wrap(NativeMethods.affineui_view_submenu(Handle, label, scope.Fn, scope.User, icon, key));
        GC.KeepAlive(this);
        return w;
    }

    public Widget MenuBrand(string title, string icon = "", string key = "")
    {
        var w = Wrap(NativeMethods.affineui_view_menu_brand(Handle, title, icon, key));
        GC.KeepAlive(this);
        return w;
    }

    public Widget MenuSpacer(string key = "")
    {
        var w = Wrap(NativeMethods.affineui_view_menu_spacer(Handle, key));
        GC.KeepAlive(this);
        return w;
    }

    /// <summary>The document being edited, centered in the bar — what a title
    /// bar shows. Pair it with <see cref="TitleBarStyle.Hidden"/> and a drag
    /// region to draw a real custom title bar.</summary>
    public Widget DocumentTitle(string text, string key = "")
    {
        var w = Wrap(NativeMethods.affineui_view_document_title(Handle, text, key));
        GC.KeepAlive(this);
        return w;
    }

    public Widget MenuMeta(string text, string key = "")
    {
        var w = Wrap(NativeMethods.affineui_view_menu_meta(Handle, text, key));
        GC.KeepAlive(this);
        return w;
    }

    public Widget TreeRow(string label, bool selected = false, int depth = 0, string key = "")
    {
        var w = Wrap(NativeMethods.affineui_view_tree_row(Handle, label, selected ? 1 : 0, depth, key));
        GC.KeepAlive(this);
        return w;
    }

    public Widget Splitter(bool horizontal, string key = "")
    {
        var w = Wrap(NativeMethods.affineui_view_splitter(Handle, horizontal ? 1 : 0, key));
        GC.KeepAlive(this);
        return w;
    }

    // ── Typed component lookups (components.h semantics) ─────────────────

    public Button ButtonAt(string name) => new(FindWidget(name));
    public Checkbox CheckboxAt(string name) => new(FindWidget(name));
    public TextField TextFieldAt(string name) => new(FindWidget(name));
    public Dropdown DropdownAt(string name) => new(FindWidget(name));
    public Slider SliderAt(string name) => new(FindWidget(name));
    public ColorField ColorFieldAt(string name) => new(FindWidget(name));
    public DockPanel DockPanelAt(string name) => new(FindWidget(name));
    public Foldout FoldoutAt(string name) => new(FindWidget(name));
}
