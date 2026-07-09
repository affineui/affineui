// Raw P/Invoke surface, 1:1 with include/affineui/c_api.h (embedded surface +
// shared value types) and include/affineui/c_api_app.h (app surface).
//
// Conventions (docs/LANGUAGE_BINDINGS.md §3):
//   - Opaque handles cross as IntPtr; the idiomatic layer wraps them in
//     SafeHandle subclasses.
//   - Strings IN are UTF-8 (source-generated marshalling); null means "empty"
//     and is always accepted by the native side.
//   - Strings OUT are returned as IntPtr; the caller copies them with
//     AffineUIRuntime.TakeString, which frees via affineui_string_free.
//     (Exception: affineui_version returns a borrowed static string.)
//   - Callback registrations take (fn, user, user_free) as raw pointers;
//     see Trampolines in AffineUIRuntime.cs.

using System.Runtime.InteropServices;

namespace AffineUI;

// ── Native POD structs (layouts mirror the C headers exactly) ────────────

[StructLayout(LayoutKind.Sequential)]
internal struct NativeColor
{
    public byte R, G, B, A;
}

/// <summary>Mirrors <c>affineui_event</c>.</summary>
[StructLayout(LayoutKind.Sequential)]
internal struct NativeEvent
{
    public int Type;       // affineui_event_type
    public int X, Y;
    public int Button;     // affineui_mouse_button
    public float WheelDx, WheelDy;
    public int Key;        // affineui_key
    public int KeyCode;
    public IntPtr Text;    // const char* (UTF-8, may be null)
    public int Shift, Ctrl, Alt, SuperKey;
}

/// <summary>Mirrors <c>affineui_dispatch_result</c>.</summary>
[StructLayout(LayoutKind.Sequential)]
internal struct NativeDispatchResult
{
    public int RedrawRequested;
    public int InvalidateView;
    public int DeferWidgetChanges;
    public int LayoutChanged;
}

/// <summary>Mirrors <c>affineui_app_config</c>.</summary>
[StructLayout(LayoutKind.Sequential)]
internal struct NativeAppConfig
{
    public IntPtr Title;               // const char*
    public int Width, Height;
    public NativeColor ClearColor;
    public int HighDpi;
    public int Vsync;
    public IntPtr DefaultFontFamily;   // const char*
    public int DefaultFontSize;
    public IntPtr AssetFolders;        // const char* const*
    public nuint AssetFolderCount;
    public int PerfOverlay;
    public int NoBundleDecius;
}

/// <summary>Mirrors <c>affineui_gpu_context</c>.</summary>
[StructLayout(LayoutKind.Sequential)]
internal struct NativeGpuContext
{
    public int Backend;                // affineui_backend
    public IntPtr D3D11Device;
    public IntPtr D3D11DeviceContext;
    public IntPtr MetalDevice;
    public IntPtr WgpuDevice;
    public int ColorFormat;            // affineui_pixel_format
    public int DepthFormat;            // affineui_pixel_format
    public int SampleCount;
}

/// <summary>Mirrors <c>affineui_init_desc</c>.</summary>
[StructLayout(LayoutKind.Sequential)]
internal struct NativeInitDesc
{
    public IntPtr Gpu;                 // const affineui_gpu_context*
    public IntPtr Allocator;           // const affineui_allocator* (unused from C#)
    public IntPtr Log;                 // affineui_log_fn
    public IntPtr LogUser;
    public IntPtr DefaultFontFamily;   // const char*
    public int DefaultFontSize;
}

/// <summary>Mirrors <c>affineui_frame_target</c>.</summary>
[StructLayout(LayoutKind.Sequential)]
internal struct NativeFrameTarget
{
    public int Width, Height;
    public float DpiScale;
    public int SampleCount;
    public int Clear;
    public int ViewportX, ViewportY, ViewportW, ViewportH;
    public IntPtr D3D11RenderView;
    public IntPtr D3D11ResolveView;
    public IntPtr D3D11DepthStencilView;
    public IntPtr MetalCurrentDrawable;
    public IntPtr MetalDepthStencilTexture;
    public IntPtr MetalMsaaColorTexture;
    public IntPtr WgpuRenderView;
    public IntPtr WgpuResolveView;
    public IntPtr WgpuDepthStencilView;
    public uint GlFramebuffer;
}

internal static partial class NativeMethods
{
    internal const string Lib = "affineui_c";

    // ── c_api.h — misc / versioning ──────────────────────────────────────

    [LibraryImport(Lib)]
    internal static partial int affineui_c_abi_version();

    /// <remarks>Returns a BORROWED static string — do not free.</remarks>
    [LibraryImport(Lib)]
    internal static partial IntPtr affineui_version();

    [LibraryImport(Lib, StringMarshalling = StringMarshalling.Utf8)]
    internal static partial int affineui_run_html(string? html);

    [LibraryImport(Lib)]
    internal static partial void affineui_string_free(IntPtr s);

    // ── c_api.h — embedded surface (affineui_ui_*) ───────────────────────

    [LibraryImport(Lib)]
    internal static partial IntPtr affineui_ui_create();

    [LibraryImport(Lib)]
    internal static partial void affineui_ui_destroy(IntPtr ui);

    [LibraryImport(Lib)]
    internal static partial void affineui_ui_init(IntPtr ui, in NativeInitDesc desc);

    [LibraryImport(Lib, StringMarshalling = StringMarshalling.Utf8)]
    internal static partial void affineui_ui_set_html(IntPtr ui, string? html);

    [LibraryImport(Lib, StringMarshalling = StringMarshalling.Utf8)]
    internal static partial void affineui_ui_set_css(IntPtr ui, string? css);

    [LibraryImport(Lib, StringMarshalling = StringMarshalling.Utf8)]
    internal static partial int affineui_ui_load_file(IntPtr ui, string? path);

    [LibraryImport(Lib)]
    internal static partial void affineui_ui_set_clear_color(IntPtr ui, byte r, byte g, byte b, byte a);

    [LibraryImport(Lib)]
    internal static partial void affineui_ui_render(IntPtr ui, in NativeFrameTarget target);

    [LibraryImport(Lib)]
    internal static partial int affineui_ui_dispatch(IntPtr ui, in NativeEvent ev);

    [LibraryImport(Lib, StringMarshalling = StringMarshalling.Utf8)]
    internal static partial void affineui_ui_on_click(IntPtr ui, string? selector,
                                                      IntPtr fn, IntPtr user, IntPtr userFree);

    [LibraryImport(Lib)]
    internal static partial int affineui_ui_hovered_cursor(IntPtr ui);

    [LibraryImport(Lib, StringMarshalling = StringMarshalling.Utf8)]
    internal static partial int affineui_ui_set_attr(IntPtr ui, string? elemId, string? name, string? value);

    [LibraryImport(Lib, StringMarshalling = StringMarshalling.Utf8)]
    internal static partial int affineui_ui_remove_attr(IntPtr ui, string? elemId, string? name);

    [LibraryImport(Lib, StringMarshalling = StringMarshalling.Utf8)]
    internal static partial int affineui_ui_set_text(IntPtr ui, string? elemId, string? text);

    [LibraryImport(Lib)]
    internal static partial void affineui_ui_content_size(IntPtr ui, out int outW, out int outH);

    [LibraryImport(Lib)]
    internal static partial int affineui_ui_needs_update(IntPtr ui);

    [LibraryImport(Lib)]
    internal static partial void affineui_ui_mark_dirty(IntPtr ui);

    [LibraryImport(Lib)]
    internal static partial void affineui_ui_reset(IntPtr ui);

    // ── c_api_app.h — App ────────────────────────────────────────────────

    [LibraryImport(Lib)]
    internal static partial void affineui_app_config_init(ref NativeAppConfig cfg);

    [LibraryImport(Lib)]
    internal static partial IntPtr affineui_app_create(in NativeAppConfig cfg);

    [LibraryImport(Lib)]
    internal static partial void affineui_app_destroy(IntPtr app);

    [LibraryImport(Lib, StringMarshalling = StringMarshalling.Utf8)]
    internal static partial void affineui_app_load_html(IntPtr app, string? html);

    [LibraryImport(Lib, StringMarshalling = StringMarshalling.Utf8)]
    internal static partial int affineui_app_load_html_file(IntPtr app, string? path);

    [LibraryImport(Lib)]
    internal static partial void affineui_app_load_view(IntPtr app, IntPtr view);

    [LibraryImport(Lib, StringMarshalling = StringMarshalling.Utf8)]
    internal static partial void affineui_app_set_stylesheet(IntPtr app, string? css, string? baseUrl);

    [LibraryImport(Lib)]
    internal static partial void affineui_app_invalidate(IntPtr app);

    [LibraryImport(Lib)]
    internal static partial void affineui_app_set_perf_overlay_enabled(IntPtr app, int enabled);

    [LibraryImport(Lib)]
    internal static partial int affineui_app_perf_overlay_enabled(IntPtr app);

    [LibraryImport(Lib)]
    internal static partial int affineui_app_dispatch(IntPtr app, in NativeEvent ev);

    [LibraryImport(Lib)]
    internal static partial int affineui_app_run(IntPtr app);

    [LibraryImport(Lib)]
    internal static partial void affineui_app_quit(IntPtr app, int code);

    [LibraryImport(Lib)]
    internal static partial void affineui_app_window_size(IntPtr app, out int outW, out int outH);

    [LibraryImport(Lib)]
    internal static partial void affineui_app_framebuffer_size(IntPtr app, out int outW, out int outH);

    [LibraryImport(Lib)]
    internal static partial float affineui_app_dpi_scale(IntPtr app);

    [LibraryImport(Lib)]
    internal static partial IntPtr affineui_app_document(IntPtr app);

    // ── c_api_app.h — Document ───────────────────────────────────────────

    [LibraryImport(Lib)]
    internal static partial IntPtr affineui_document_create();

    [LibraryImport(Lib)]
    internal static partial void affineui_document_destroy(IntPtr doc);

    [LibraryImport(Lib, StringMarshalling = StringMarshalling.Utf8)]
    internal static partial void affineui_document_set_html(IntPtr doc, string? html);

    [LibraryImport(Lib, StringMarshalling = StringMarshalling.Utf8)]
    internal static partial void affineui_document_set_user_stylesheet(IntPtr doc, string? css, string? baseUrl);

    [LibraryImport(Lib)]
    internal static partial void affineui_document_reload_stylesheets(IntPtr doc);

    [LibraryImport(Lib)]
    internal static partial void affineui_document_layout(IntPtr doc, int viewportWidth, int viewportHeight);

    [LibraryImport(Lib)]
    internal static partial void affineui_document_content_size(IntPtr doc, out int outW, out int outH);

    [LibraryImport(Lib, StringMarshalling = StringMarshalling.Utf8)]
    internal static partial int affineui_document_set_attribute_by_id(IntPtr doc, string? elemId, string? name, string? value);

    [LibraryImport(Lib, StringMarshalling = StringMarshalling.Utf8)]
    internal static partial int affineui_document_remove_attribute_by_id(IntPtr doc, string? elemId, string? name);

    [LibraryImport(Lib, StringMarshalling = StringMarshalling.Utf8)]
    internal static partial int affineui_document_set_text_by_id(IntPtr doc, string? elemId, string? text);

    [LibraryImport(Lib)]
    internal static partial void affineui_document_dispatch(IntPtr doc, in NativeEvent ev, out NativeDispatchResult result);

    [LibraryImport(Lib)]
    internal static partial void affineui_document_attach_script(IntPtr doc, int script);

    [LibraryImport(Lib)]
    internal static partial void affineui_document_detach_script(IntPtr doc, int script);

    [LibraryImport(Lib)]
    internal static partial int affineui_document_hovered_cursor(IntPtr doc);

    // ── c_api_app.h — View ───────────────────────────────────────────────

    [LibraryImport(Lib)]
    internal static partial IntPtr affineui_view_create(int theme);

    [LibraryImport(Lib)]
    internal static partial void affineui_view_destroy(IntPtr view);

    [LibraryImport(Lib)]
    internal static partial void affineui_view_clear(IntPtr view);

    [LibraryImport(Lib)]
    internal static partial void affineui_view_begin(IntPtr view);

    [LibraryImport(Lib)]
    internal static partial void affineui_view_end(IntPtr view);

    [LibraryImport(Lib)]
    internal static partial void affineui_view_set_theme(IntPtr view, int theme);

    [LibraryImport(Lib)]
    internal static partial int affineui_view_get_theme(IntPtr view);

    [LibraryImport(Lib, StringMarshalling = StringMarshalling.Utf8)]
    internal static partial void affineui_view_set_framework_version(IntPtr view, string? version);

    [LibraryImport(Lib, StringMarshalling = StringMarshalling.Utf8)]
    internal static partial void affineui_view_selector(IntPtr view, string? name, string? value);

    [LibraryImport(Lib)]
    internal static partial IntPtr affineui_view_to_html_fragment(IntPtr view);

    [LibraryImport(Lib)]
    internal static partial IntPtr affineui_view_to_html_document(IntPtr view);

    [LibraryImport(Lib, StringMarshalling = StringMarshalling.Utf8)]
    internal static partial IntPtr affineui_view_heading(IntPtr view, int level, string? text, string? classes, string? key);

    [LibraryImport(Lib, StringMarshalling = StringMarshalling.Utf8)]
    internal static partial IntPtr affineui_view_paragraph(IntPtr view, string? text, string? classes, string? key);

    [LibraryImport(Lib, StringMarshalling = StringMarshalling.Utf8)]
    internal static partial IntPtr affineui_view_text(IntPtr view, string? text, string? key);

    [LibraryImport(Lib, StringMarshalling = StringMarshalling.Utf8)]
    internal static partial IntPtr affineui_view_html(IntPtr view, string? markup, string? key);

    [LibraryImport(Lib, StringMarshalling = StringMarshalling.Utf8)]
    internal static partial IntPtr affineui_view_button(IntPtr view, string? label, int primary, string? key);

    [LibraryImport(Lib, StringMarshalling = StringMarshalling.Utf8)]
    internal static partial IntPtr affineui_view_checkbox(IntPtr view, string? label, int isChecked, string? key);

    [LibraryImport(Lib, StringMarshalling = StringMarshalling.Utf8)]
    internal static partial IntPtr affineui_view_toggle(IntPtr view, string? label, int on, string? key);

    [LibraryImport(Lib, StringMarshalling = StringMarshalling.Utf8)]
    internal static partial IntPtr affineui_view_input(IntPtr view, string? label, string? value, string? type, string? key);

    [LibraryImport(Lib, StringMarshalling = StringMarshalling.Utf8)]
    internal static partial IntPtr affineui_view_password(IntPtr view, string? label, string? value, string? key);

    [LibraryImport(Lib, StringMarshalling = StringMarshalling.Utf8)]
    internal static partial IntPtr affineui_view_textarea(IntPtr view, string? label, string? value, int rows, string? key);

    [LibraryImport(Lib, StringMarshalling = StringMarshalling.Utf8)]
    internal static partial IntPtr affineui_view_dropdown(IntPtr view, string? label, IntPtr options, nuint optionCount, string? selected, string? key);

    [LibraryImport(Lib, StringMarshalling = StringMarshalling.Utf8)]
    internal static partial IntPtr affineui_view_button_group(IntPtr view, string? label, IntPtr options, nuint optionCount, string? selected, string? key);

    [LibraryImport(Lib, StringMarshalling = StringMarshalling.Utf8)]
    internal static partial IntPtr affineui_view_slider(IntPtr view, string? label, double value, double min, double max, string? key);

    [LibraryImport(Lib, StringMarshalling = StringMarshalling.Utf8)]
    internal static partial IntPtr affineui_view_knob(IntPtr view, string? label, double value, double min, double max, int bipolar, string? key);

    [LibraryImport(Lib, StringMarshalling = StringMarshalling.Utf8)]
    internal static partial IntPtr affineui_view_combo(IntPtr view, string? label, double value, double step, string? key);

    [LibraryImport(Lib, StringMarshalling = StringMarshalling.Utf8)]
    internal static partial IntPtr affineui_view_color_field(IntPtr view, string? label, string? value, IntPtr swatches, nuint swatchCount, string? key);

    [LibraryImport(Lib, StringMarshalling = StringMarshalling.Utf8)]
    internal static partial IntPtr affineui_view_colorfield(IntPtr view, string? label, string? value, string? key);

    [LibraryImport(Lib, StringMarshalling = StringMarshalling.Utf8)]
    internal static partial IntPtr affineui_view_container(IntPtr view, string? classes, string? key, IntPtr build, IntPtr user);

    [LibraryImport(Lib, StringMarshalling = StringMarshalling.Utf8)]
    internal static partial IntPtr affineui_view_element(IntPtr view, string? tag, string? classes, string? key, IntPtr build, IntPtr user);

    [LibraryImport(Lib, StringMarshalling = StringMarshalling.Utf8)]
    internal static partial IntPtr affineui_view_panel(IntPtr view, string? key, IntPtr build, IntPtr user);

    [LibraryImport(Lib, StringMarshalling = StringMarshalling.Utf8)]
    internal static partial IntPtr affineui_view_card(IntPtr view, string? title, string? classes, string? key, IntPtr build, IntPtr user);

    [LibraryImport(Lib, StringMarshalling = StringMarshalling.Utf8)]
    internal static partial IntPtr affineui_view_toolbar(IntPtr view, string? key, IntPtr build, IntPtr user);

    [LibraryImport(Lib, StringMarshalling = StringMarshalling.Utf8)]
    internal static partial IntPtr affineui_view_menu_bar(IntPtr view, string? key, IntPtr build, IntPtr user);

    [LibraryImport(Lib, StringMarshalling = StringMarshalling.Utf8)]
    internal static partial IntPtr affineui_view_status_bar(IntPtr view, string? key, IntPtr build, IntPtr user);

    [LibraryImport(Lib, StringMarshalling = StringMarshalling.Utf8)]
    internal static partial IntPtr affineui_view_tree(IntPtr view, string? key, IntPtr build, IntPtr user);

    [LibraryImport(Lib, StringMarshalling = StringMarshalling.Utf8)]
    internal static partial IntPtr affineui_view_foldout(IntPtr view, string? title, int expanded, string? key, IntPtr build, IntPtr user);

    [LibraryImport(Lib, StringMarshalling = StringMarshalling.Utf8)]
    internal static partial IntPtr affineui_view_toolbar_separator(IntPtr view, string? key);

    [LibraryImport(Lib, StringMarshalling = StringMarshalling.Utf8)]
    internal static partial IntPtr affineui_view_icon_button(IntPtr view, string? icon, string? key);

    [LibraryImport(Lib, StringMarshalling = StringMarshalling.Utf8)]
    internal static partial IntPtr affineui_view_menu_button(IntPtr view, string? label, IntPtr build, IntPtr user, string? key);

    [LibraryImport(Lib, StringMarshalling = StringMarshalling.Utf8)]
    internal static partial IntPtr affineui_view_menu_item(IntPtr view, string? label, string? icon, string? shortcut, string? key);

    [LibraryImport(Lib, StringMarshalling = StringMarshalling.Utf8)]
    internal static partial IntPtr affineui_view_menu_separator(IntPtr view, string? key);

    [LibraryImport(Lib, StringMarshalling = StringMarshalling.Utf8)]
    internal static partial IntPtr affineui_view_submenu(IntPtr view, string? label, IntPtr build, IntPtr user, string? icon, string? key);

    [LibraryImport(Lib, StringMarshalling = StringMarshalling.Utf8)]
    internal static partial IntPtr affineui_view_menu_brand(IntPtr view, string? title, string? icon, string? key);

    [LibraryImport(Lib, StringMarshalling = StringMarshalling.Utf8)]
    internal static partial IntPtr affineui_view_menu_spacer(IntPtr view, string? key);

    [LibraryImport(Lib, StringMarshalling = StringMarshalling.Utf8)]
    internal static partial IntPtr affineui_view_menu_meta(IntPtr view, string? text, string? key);

    [LibraryImport(Lib, StringMarshalling = StringMarshalling.Utf8)]
    internal static partial IntPtr affineui_view_tree_row(IntPtr view, string? label, int selected, int depth, string? key);

    [LibraryImport(Lib, StringMarshalling = StringMarshalling.Utf8)]
    internal static partial IntPtr affineui_view_splitter(IntPtr view, int horizontal, string? key);

    [LibraryImport(Lib, StringMarshalling = StringMarshalling.Utf8)]
    internal static partial IntPtr affineui_view_find_widget(IntPtr view, string? name);

    // ── c_api_app.h — Widget ─────────────────────────────────────────────

    [LibraryImport(Lib)]
    internal static partial void affineui_widget_destroy(IntPtr w);

    [LibraryImport(Lib)]
    internal static partial int affineui_widget_valid(IntPtr w);

    [LibraryImport(Lib)]
    internal static partial int affineui_widget_get_kind(IntPtr w);

    [LibraryImport(Lib)]
    internal static partial IntPtr affineui_widget_name(IntPtr w);

    [LibraryImport(Lib, StringMarshalling = StringMarshalling.Utf8)]
    internal static partial IntPtr affineui_widget_attr(IntPtr w, string? name, string? fallback);

    [LibraryImport(Lib)]
    internal static partial IntPtr affineui_widget_text(IntPtr w);

    [LibraryImport(Lib, StringMarshalling = StringMarshalling.Utf8)]
    internal static partial int affineui_widget_has_attr(IntPtr w, string? name);

    [LibraryImport(Lib, StringMarshalling = StringMarshalling.Utf8)]
    internal static partial void affineui_widget_set_text(IntPtr w, string? text);

    [LibraryImport(Lib, StringMarshalling = StringMarshalling.Utf8)]
    internal static partial void affineui_widget_set_attr(IntPtr w, string? name, string? value);

    [LibraryImport(Lib, StringMarshalling = StringMarshalling.Utf8)]
    internal static partial void affineui_widget_remove_attr(IntPtr w, string? name);

    [LibraryImport(Lib, StringMarshalling = StringMarshalling.Utf8)]
    internal static partial void affineui_widget_set_selector(IntPtr w, string? name, string? value);

    [LibraryImport(Lib, StringMarshalling = StringMarshalling.Utf8)]
    internal static partial void affineui_widget_add_class(IntPtr w, string? classes);

    [LibraryImport(Lib)]
    internal static partial void affineui_widget_clear(IntPtr w);

    [LibraryImport(Lib)]
    internal static partial void affineui_widget_on_click(IntPtr w, IntPtr fn, IntPtr user, IntPtr userFree);

    [LibraryImport(Lib)]
    internal static partial void affineui_widget_on_change(IntPtr w, IntPtr fn, IntPtr user, IntPtr userFree);

    [LibraryImport(Lib)]
    internal static partial void affineui_widget_append(IntPtr w, IntPtr build, IntPtr user);

    [LibraryImport(Lib)]
    internal static partial void affineui_widget_replace(IntPtr w, IntPtr build, IntPtr user);

    [LibraryImport(Lib, StringMarshalling = StringMarshalling.Utf8)]
    internal static partial IntPtr affineui_widget_find_widget(IntPtr w, string? name);
}
