// Public value types and enums. Enum values mirror the C ABI enums exactly
// (which in turn are static_assert-pinned to the C++ enums).

using System.Runtime.InteropServices;

namespace AffineUI;

/// <summary>CSS framework theme a <see cref="View"/> targets.
/// Mirrors <c>affineui_view_theme</c>.</summary>
public enum Theme
{
    Plain = 0,
    Bootstrap = 1,
    Decius = 2,
}

/// <summary>Widget node kind. Mirrors <c>affineui_widget_kind</c>;
/// <see cref="None"/> is the C-only "no node attached" sentinel.</summary>
public enum WidgetKind
{
    None = -1,
    Root = 0,
    Container = 1,
    Text = 2,
    RawHtml = 3,
    Heading = 4,
    Panel = 5,
    Button = 6,
    Checkbox = 7,
    Slider = 8,
    Knob = 9,
    TextInput = 10,
    TextArea = 11,
    Dropdown = 12,
    ButtonGroup = 13,
    VirtualList = 14,
    Card = 15,
}

/// <summary>Mirrors <c>affineui_event_type</c>.</summary>
public enum EventType
{
    None = 0,
    MouseMove = 1,
    MouseDown = 2,
    MouseUp = 3,
    MouseWheel = 4,
    KeyDown = 5,
    KeyUp = 6,
    TextInput = 7,
    Resize = 8,
    FocusLost = 9,
    FocusGained = 10,
    /// <summary>IME preedit update: <see cref="Event.Text"/> carries the
    /// uncommitted composition string (empty = composition ended), shown
    /// inline at the caret but never written to the control's value.
    /// Committed text still arrives as <see cref="TextInput"/>.</summary>
    Composition = 11,
}

/// <summary>Mirrors <c>affineui_mouse_button</c>.</summary>
public enum MouseButton
{
    Left = 0,
    Right = 1,
    Middle = 2,
}

/// <summary>Mirrors <c>affineui_key</c>. Printable text arrives as
/// <see cref="EventType.TextInput"/>, not as key codes.</summary>
public enum Key
{
    Unknown = 0,
    Escape = 1,
    Tab = 2,
    Enter = 3,
    Backspace = 4,
    Delete = 5,
    ArrowLeft = 6,
    ArrowRight = 7,
    ArrowUp = 8,
    ArrowDown = 9,
    Home = 10,
    End = 11,
    A = 12, B = 13, C = 14, D = 15, E = 16, F = 17, G = 18, H = 19, I = 20,
    J = 21, K = 22, L = 23, M = 24, N = 25, O = 26, P = 27, Q = 28, R = 29,
    S = 30, T = 31, U = 32, V = 33, W = 34, X = 35, Y = 36, Z = 37,
    Digit0 = 38, Digit1 = 39, Digit2 = 40, Digit3 = 41, Digit4 = 42,
    Digit5 = 43, Digit6 = 44, Digit7 = 45, Digit8 = 46, Digit9 = 47,
}

/// <summary>Cursor the OS should display (mapping documented on
/// <c>affineui_ui_hovered_cursor</c>).</summary>
public enum Cursor
{
    Default = 0,
    Pointer = 1,
    Text = 2,
    Crosshair = 3,
    Move = 4,
    NotAllowed = 5,
    EwResize = 6,
    NsResize = 7,
    NwseResize = 8,
}

/// <summary>Optional document behavior scripts
/// (<c>affineui_document_attach_script</c>).</summary>
public enum DocumentScript
{
    UiControls = 0,
}

/// <summary>Native log severity. Mirrors <c>affineui_log_level</c>.</summary>
public enum LogLevel
{
    Debug = 0,
    Info = 1,
    Warn = 2,
    Error = 3,
}

/// <summary>
/// Typed-component validity (components.h semantics): a component is fully
/// usable only when <see cref="Valid"/>; a <see cref="WrongType"/> component
/// stays attached to the node it found (generic attribute/text access works,
/// typed accessors are inert); <see cref="NotPresent"/> means no node resolved.
/// </summary>
public enum Validity
{
    NotPresent = 0,
    WrongType = 1,
    Valid = 2,
}

/// <summary>RGBA color, 8 bits per channel.</summary>
public readonly record struct Color(byte R, byte G, byte B, byte A = 255);

/// <summary>Integer width/height pair.</summary>
public readonly record struct Size(int Width, int Height);

/// <summary>Result of dispatching an event to a <see cref="Document"/>.
/// Mirrors <c>affineui_dispatch_result</c>.</summary>
public readonly struct DispatchResult
{
    public bool RedrawRequested { get; init; }
    public bool InvalidateView { get; init; }
    public bool DeferWidgetChanges { get; init; }
    public bool LayoutChanged { get; init; }

    internal static DispatchResult FromNative(in NativeDispatchResult n) => new()
    {
        RedrawRequested = n.RedrawRequested != 0,
        InvalidateView = n.InvalidateView != 0,
        DeferWidgetChanges = n.DeferWidgetChanges != 0,
        LayoutChanged = n.LayoutChanged != 0,
    };
}

/// <summary>
/// Input event value object. Mirrors <c>affineui_event</c>; shared by both
/// operating modes (App.Dispatch / Document.Dispatch / Embedded.Ui.Dispatch).
/// </summary>
public struct Event
{
    public EventType Type;
    /// <summary>Pointer position in CSS/layout points.</summary>
    public int X, Y;
    public MouseButton Button;
    public float WheelDx, WheelDy;
    public Key Key;
    /// <summary>Platform-native scancode (debug / passthrough).</summary>
    public int KeyCode;
    /// <summary>UTF-8 text; read for <see cref="EventType.TextInput"/>
    /// (committed) and <see cref="EventType.Composition"/> (preedit).</summary>
    public string? Text;
    /// <summary>Composition only — IME caret as a byte offset into
    /// <see cref="Text"/> (negative = end of preedit).</summary>
    public int CompositionCursor;
    /// <summary>Composition only — the IME's active clause as byte offsets
    /// into <see cref="Text"/> (equal = none reported).</summary>
    public int CompositionClauseBegin, CompositionClauseEnd;
    public bool Shift, Ctrl, Alt, Super;

    public static Event MouseMove(int x, int y) =>
        new() { Type = EventType.MouseMove, X = x, Y = y };

    public static Event MouseDown(int x, int y, MouseButton button = MouseButton.Left) =>
        new() { Type = EventType.MouseDown, X = x, Y = y, Button = button };

    public static Event MouseUp(int x, int y, MouseButton button = MouseButton.Left) =>
        new() { Type = EventType.MouseUp, X = x, Y = y, Button = button };

    public static Event MouseWheel(int x, int y, float dx, float dy) =>
        new() { Type = EventType.MouseWheel, X = x, Y = y, WheelDx = dx, WheelDy = dy };

    public static Event KeyDown(Key key, bool shift = false, bool ctrl = false,
                                bool alt = false, bool super = false) =>
        new() { Type = EventType.KeyDown, Key = key, Shift = shift, Ctrl = ctrl, Alt = alt, Super = super };

    public static Event KeyUp(Key key, bool shift = false, bool ctrl = false,
                              bool alt = false, bool super = false) =>
        new() { Type = EventType.KeyUp, Key = key, Shift = shift, Ctrl = ctrl, Alt = alt, Super = super };

    public static Event TextInput(string text) =>
        new() { Type = EventType.TextInput, Text = text };

    /// <summary>IME preedit update; <paramref name="cursor"/> is a byte
    /// offset into <paramref name="preedit"/> (negative = end).</summary>
    public static Event Composition(string preedit, int cursor = -1) =>
        new() { Type = EventType.Composition, Text = preedit, CompositionCursor = cursor };

    public static Event Resize() => new() { Type = EventType.Resize };
    public static Event FocusLost() => new() { Type = EventType.FocusLost };
    public static Event FocusGained() => new() { Type = EventType.FocusGained };

    /// <summary>
    /// Marshals to the native layout. When <see cref="Text"/> is non-null the
    /// caller must free <paramref name="textPtr"/> (Marshal.FreeCoTaskMem)
    /// after the native call returns.
    /// </summary>
    internal readonly NativeEvent ToNative(out IntPtr textPtr)
    {
        textPtr = Text is null ? IntPtr.Zero : Marshal.StringToCoTaskMemUTF8(Text);
        return new NativeEvent
        {
            Type = (int)Type,
            X = X,
            Y = Y,
            Button = (int)Button,
            WheelDx = WheelDx,
            WheelDy = WheelDy,
            Key = (int)Key,
            KeyCode = KeyCode,
            Text = textPtr,
            Shift = Shift ? 1 : 0,
            Ctrl = Ctrl ? 1 : 0,
            Alt = Alt ? 1 : 0,
            SuperKey = Super ? 1 : 0,
            CompositionCursor = CompositionCursor,
            CompositionClauseBegin = CompositionClauseBegin,
            CompositionClauseEnd = CompositionClauseEnd,
        };
    }
}
