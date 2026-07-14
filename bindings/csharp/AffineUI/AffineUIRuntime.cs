// Runtime plumbing shared by both operating modes: native library resolution,
// the C ABI version gate, the callback trampolines (GCHandle-based, per
// docs/LANGUAGE_BINDINGS.md §5.2), and string helpers.

using System.Diagnostics;
using System.Reflection;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;

namespace AffineUI;

/// <summary>
/// Global runtime services for the AffineUI wrapper: native library loading,
/// ABI version verification, and callback exception routing.
/// </summary>
public static class AffineUIRuntime
{
    /// <summary>The C ABI version this wrapper was written against
    /// (<c>AFFINEUI_C_ABI_VERSION</c>).</summary>
    public const int ExpectedAbiVersion = 2;

    private static readonly object Gate = new();
    private static volatile bool _loaded;
    private static int _uiThreadId = -1;
    private static long _releasedCallbacks;

    /// <summary>
    /// Called when a managed callback invoked from native code throws.
    /// Exceptions must never unwind across the FFI, so they are routed here
    /// instead. Default: write to <see cref="Console.Error"/>.
    /// </summary>
    public static Action<Exception> OnCallbackException { get; set; } = static ex =>
        Console.Error.WriteLine($"[AffineUI] unhandled exception in managed callback: {ex}");

    /// <summary>The native AffineUI version string.</summary>
    public static string Version
    {
        get
        {
            EnsureLoaded();
            // affineui_version returns a borrowed static string — no free.
            return Marshal.PtrToStringUTF8(NativeMethods.affineui_version()) ?? string.Empty;
        }
    }

    /// <summary>The C ABI version reported by the loaded native library.</summary>
    public static int AbiVersion
    {
        get
        {
            EnsureLoaded();
            return NativeMethods.affineui_c_abi_version();
        }
    }

    /// <summary>
    /// Number of native-held callback closures released so far (each is one
    /// exactly-once <c>user_free</c> call from the core). Useful in tests to
    /// assert that disposing a view released its handlers.
    /// </summary>
    public static long ReleasedCallbackCount => Interlocked.Read(ref _releasedCallbacks);

    /// <summary>Convenience: open a native window, show <paramref name="html"/>,
    /// run to completion. Blocks the calling thread.</summary>
    public static int RunHtml(string html)
    {
        EnsureLoaded();
        return NativeMethods.affineui_run_html(html);
    }

    internal static void EnsureLoaded()
    {
        if (_loaded) return;
        lock (Gate)
        {
            if (_loaded) return;
            NativeLibrary.SetDllImportResolver(typeof(AffineUIRuntime).Assembly, ResolveNativeLibrary);

            int abi;
            try
            {
                abi = NativeMethods.affineui_c_abi_version();
            }
            catch (DllNotFoundException ex)
            {
                throw new DllNotFoundException(
                    "The AffineUI native library 'affineui_c' could not be found. " +
                    "Set the AFFINEUI_NATIVE_DIR environment variable to the directory " +
                    "containing affineui_c.dll / libaffineui_c.so / libaffineui_c.dylib, " +
                    "or place it next to the application.", ex);
            }

            if (abi != ExpectedAbiVersion)
            {
                throw new NotSupportedException(
                    $"AffineUI C ABI version mismatch: the native library reports version {abi} " +
                    $"but this wrapper expects {ExpectedAbiVersion}. Update the AffineUI .NET " +
                    "package or rebuild affineui_c so the versions match.");
            }

            _uiThreadId = Environment.CurrentManagedThreadId;
            _loaded = true;
        }
    }

    /// <summary>
    /// Debug-only guard for the single-threaded contract (§3.6): the whole
    /// surface must be used from one thread.
    /// </summary>
    [Conditional("DEBUG")]
    internal static void CheckThread()
    {
        if (_loaded && _uiThreadId != Environment.CurrentManagedThreadId)
        {
            throw new InvalidOperationException(
                "AffineUI objects must be created and used on a single thread " +
                $"(first used on managed thread {_uiThreadId}, now on " +
                $"{Environment.CurrentManagedThreadId}).");
        }
    }

    internal static void ReportCallbackException(Exception ex)
    {
        try { OnCallbackException?.Invoke(ex); }
        catch { /* the exception sink itself must never unwind into native */ }
    }

    internal static void NoteCallbackReleased() => Interlocked.Increment(ref _releasedCallbacks);

    /// <summary>Copies a native heap string and frees it with
    /// <c>affineui_string_free</c>.</summary>
    internal static string TakeString(IntPtr p, string fallback = "")
    {
        if (p == IntPtr.Zero) return fallback;
        try { return Marshal.PtrToStringUTF8(p) ?? fallback; }
        finally { NativeMethods.affineui_string_free(p); }
    }

    private static IntPtr ResolveNativeLibrary(string name, Assembly assembly, DllImportSearchPath? searchPath)
    {
        if (!name.Equals(NativeMethods.Lib, StringComparison.OrdinalIgnoreCase))
            return IntPtr.Zero;

        foreach (var dir in CandidateDirectories())
        {
            foreach (var file in CandidateFileNames())
            {
                var full = Path.Combine(dir, file);
                if (File.Exists(full) && NativeLibrary.TryLoad(full, out var handle))
                    return handle;
            }
        }

        // Fall through to the default probing (app dir, runtimes/<rid>/native).
        return IntPtr.Zero;
    }

    private static IEnumerable<string> CandidateDirectories()
    {
        var env = Environment.GetEnvironmentVariable("AFFINEUI_NATIVE_DIR");
        if (!string.IsNullOrWhiteSpace(env))
            yield return env;
        yield return AppContext.BaseDirectory;
    }

    private static IEnumerable<string> CandidateFileNames()
    {
        if (OperatingSystem.IsWindows())
        {
            yield return "affineui_c.dll";
        }
        else if (OperatingSystem.IsMacOS())
        {
            yield return "libaffineui_c.dylib";
            yield return "affineui_c.dylib";
        }
        else
        {
            yield return "libaffineui_c.so";
            yield return "affineui_c.so";
        }
    }
}

/// <summary>
/// Static [UnmanagedCallersOnly] trampolines — one per callback shape in the
/// C ABI. The managed closure is boxed in a GCHandle passed as `user`; the
/// core calls `user_free` exactly once, which frees the GCHandle. The function
/// pointers themselves are static methods and therefore immune to GC.
/// Every body is wrapped in try/catch: managed exceptions never cross the FFI.
/// </summary>
internal static unsafe class Trampolines
{
    internal static readonly IntPtr Click =
        (IntPtr)(delegate* unmanaged[Cdecl]<void*, void>)&OnClick;

    internal static readonly IntPtr Change =
        (IntPtr)(delegate* unmanaged[Cdecl]<void*, byte*, void>)&OnChange;

    internal static readonly IntPtr Build =
        (IntPtr)(delegate* unmanaged[Cdecl]<void*, void*, void>)&OnBuild;

    internal static readonly IntPtr EventCapture =
        (IntPtr)(delegate* unmanaged[Cdecl]<void*, NativeEvent*, int>)&OnEventCapture;

    internal static readonly IntPtr FreeUser =
        (IntPtr)(delegate* unmanaged[Cdecl]<void*, void>)&OnFreeUser;

    internal static readonly IntPtr Log =
        (IntPtr)(delegate* unmanaged[Cdecl]<int, byte*, void*, void>)&OnLog;

    // ── Dock providers ───────────────────────────────────────────────
    // Each is a (fn, user, user_free) triple; the GCHandle is released by the
    // engine's user_free, exactly once, when the view drops the callback.

    internal static readonly IntPtr DockSize =
        (IntPtr)(delegate* unmanaged[Cdecl]<void*, byte*, int>)&OnDockSize;

    internal static readonly IntPtr DockActiveTab =
        (IntPtr)(delegate* unmanaged[Cdecl]<void*, byte*, byte*>)&OnDockActiveTab;

    internal static readonly IntPtr DockPlacement =
        (IntPtr)(delegate* unmanaged[Cdecl]<void*, byte*, NativeMethods.affineui_dock_placement*, void>)&OnDockPlacement;

    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
    private static int OnDockSize(void* user, byte* paneId)
    {
        try
        {
            if (GCHandle.FromIntPtr((IntPtr)user).Target is Func<string, int> f)
                return f(Utf8(paneId));
        }
        catch (Exception ex) { AffineUIRuntime.ReportCallbackException(ex); }
        return 0;  // <= 0 => fall back to the declared size
    }

    /// <summary>
    /// The engine copies the returned string immediately, but it must stay valid
    /// until this returns — so the marshalled buffer is parked in the state
    /// object rather than being a temporary that the GC could move or collect.
    /// </summary>
    internal sealed class DockTabState
    {
        public Func<string, string> Fn = _ => string.Empty;
        public IntPtr Last;  // unmanaged UTF-8, freed on replace/dispose

        public IntPtr Marshal(string s)
        {
            if (Last != IntPtr.Zero) System.Runtime.InteropServices.Marshal.FreeCoTaskMem(Last);
            Last = System.Runtime.InteropServices.Marshal.StringToCoTaskMemUTF8(s);
            return Last;
        }

        public void FreeLast()
        {
            if (Last != IntPtr.Zero) System.Runtime.InteropServices.Marshal.FreeCoTaskMem(Last);
            Last = IntPtr.Zero;
        }
    }

    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
    private static byte* OnDockActiveTab(void* user, byte* paneId)
    {
        try
        {
            if (GCHandle.FromIntPtr((IntPtr)user).Target is DockTabState st)
                return (byte*)st.Marshal(st.Fn(Utf8(paneId)));
        }
        catch (Exception ex) { AffineUIRuntime.ReportCallbackException(ex); }
        return null;  // null => the primary panel
    }

    /// <summary>Same story as <see cref="DockTabState"/>, for the placement's parent id.</summary>
    internal sealed class DockPlacementState
    {
        public Func<string, DockPlacement?> Fn = _ => null;
        public IntPtr LastParent;

        public IntPtr MarshalParent(string s)
        {
            if (LastParent != IntPtr.Zero) System.Runtime.InteropServices.Marshal.FreeCoTaskMem(LastParent);
            LastParent = System.Runtime.InteropServices.Marshal.StringToCoTaskMemUTF8(s);
            return LastParent;
        }

        public void FreeLast()
        {
            if (LastParent != IntPtr.Zero) System.Runtime.InteropServices.Marshal.FreeCoTaskMem(LastParent);
            LastParent = IntPtr.Zero;
        }
    }

    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
    private static void OnDockPlacement(void* user, byte* panelId,
                                        NativeMethods.affineui_dock_placement* outPlacement)
    {
        try
        {
            if (GCHandle.FromIntPtr((IntPtr)user).Target is DockPlacementState st)
            {
                var p = st.Fn(Utf8(panelId));
                if (p is null)
                {
                    // Present = 0 => "no override; use the declared DockLocation".
                    outPlacement->Present = 0;
                    return;
                }
                outPlacement->Present = 1;
                outPlacement->Floating = p.Floating ? 1 : 0;
                outPlacement->Parent = st.MarshalParent(p.Parent ?? string.Empty);
                outPlacement->Side = (int)p.Side;
                outPlacement->Size = p.Size;
                outPlacement->X = p.X;
                outPlacement->Y = p.Y;
                outPlacement->W = p.W;
                outPlacement->H = p.H;
                return;
            }
        }
        catch (Exception ex) { AffineUIRuntime.ReportCallbackException(ex); }
        outPlacement->Present = 0;
    }

    private static string Utf8(byte* p) =>
        p is null ? string.Empty : Marshal.PtrToStringUTF8((IntPtr)p) ?? string.Empty;

    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
    private static void OnClick(void* user)
    {
        try
        {
            if (GCHandle.FromIntPtr((IntPtr)user).Target is Action action)
                action();
        }
        catch (Exception ex) { AffineUIRuntime.ReportCallbackException(ex); }
    }

    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
    private static void OnChange(void* user, byte* value)
    {
        try
        {
            string v = value is null ? string.Empty
                                     : Marshal.PtrToStringUTF8((IntPtr)value) ?? string.Empty;
            if (GCHandle.FromIntPtr((IntPtr)user).Target is Action<string> action)
                action(v);
        }
        catch (Exception ex) { AffineUIRuntime.ReportCallbackException(ex); }
    }

    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
    private static void OnBuild(void* user, void* view)
    {
        try
        {
            if (GCHandle.FromIntPtr((IntPtr)user).Target is Action<View> action)
                action(View.Borrowed((IntPtr)view));
        }
        catch (Exception ex) { AffineUIRuntime.ReportCallbackException(ex); }
    }

    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
    private static int OnEventCapture(void* user, NativeEvent* ev)
    {
        try
        {
            if (ev is not null &&
                GCHandle.FromIntPtr((IntPtr)user).Target is Func<Event, bool> handler)
                return handler(Event.FromNative(in *ev)) ? 1 : 0;
        }
        catch (Exception ex) { AffineUIRuntime.ReportCallbackException(ex); }
        return 0;
    }

    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
    private static void OnFreeUser(void* user)
    {
        try
        {
            var handle = GCHandle.FromIntPtr((IntPtr)user);
            // The dock tab/placement providers park an unmanaged UTF-8 buffer in
            // their state (the engine borrows the pointer across the callback
            // return). Release it here, or it leaks with the handle.
            switch (handle.Target)
            {
                case DockTabState tab: tab.FreeLast(); break;
                case DockPlacementState pl: pl.FreeLast(); break;
            }
            handle.Free();
            AffineUIRuntime.NoteCallbackReleased();
        }
        catch (Exception ex) { AffineUIRuntime.ReportCallbackException(ex); }
    }

    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
    private static void OnLog(int level, byte* msg, void* user)
    {
        try
        {
            string m = msg is null ? string.Empty
                                   : Marshal.PtrToStringUTF8((IntPtr)msg) ?? string.Empty;
            if (GCHandle.FromIntPtr((IntPtr)user).Target is Action<LogLevel, string> action)
                action((LogLevel)level, m);
        }
        catch (Exception ex) { AffineUIRuntime.ReportCallbackException(ex); }
    }
}

/// <summary>
/// Scope for the synchronous build callbacks (affineui_build_fn). The C ABI
/// takes only (build, user) for these — they are invoked synchronously inside
/// the registering call and NOT retained — so the wrapper owns the GCHandle
/// and frees it when the native call returns.
/// </summary>
internal readonly struct BuildScope : IDisposable
{
    public readonly IntPtr Fn;
    public readonly IntPtr User;
    private readonly GCHandle _handle;

    private BuildScope(IntPtr fn, IntPtr user, GCHandle handle)
    {
        Fn = fn;
        User = user;
        _handle = handle;
    }

    public static BuildScope Create(Action<View>? build)
    {
        if (build is null) return default;
        var handle = GCHandle.Alloc(build);
        return new BuildScope(Trampolines.Build, GCHandle.ToIntPtr(handle), handle);
    }

    public void Dispose()
    {
        if (_handle.IsAllocated) _handle.Free();
    }
}

/// <summary>
/// Temporary unmanaged `const char* const*` array for list-in parameters
/// (dropdown options, swatches, asset folders).
/// </summary>
internal sealed class Utf8StringArray : IDisposable
{
    private readonly List<IntPtr> _strings = new();
    private IntPtr _array;

    public IntPtr Pointer => _array;
    public nuint Count { get; }

    public Utf8StringArray(IReadOnlyList<string>? items)
    {
        if (items is null || items.Count == 0) return;
        Count = (nuint)items.Count;
        _array = Marshal.AllocCoTaskMem(items.Count * IntPtr.Size);
        for (int i = 0; i < items.Count; i++)
        {
            IntPtr s = Marshal.StringToCoTaskMemUTF8(items[i] ?? string.Empty);
            _strings.Add(s);
            Marshal.WriteIntPtr(_array, i * IntPtr.Size, s);
        }
    }

    public void Dispose()
    {
        foreach (var s in _strings) Marshal.FreeCoTaskMem(s);
        _strings.Clear();
        if (_array != IntPtr.Zero)
        {
            Marshal.FreeCoTaskMem(_array);
            _array = IntPtr.Zero;
        }
    }
}
