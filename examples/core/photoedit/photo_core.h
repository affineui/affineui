// Raster photo-editing core for the Decius Photo Edit sample.
//
// A faithful C++ port of the decius-css/samples/decius-photo web engine
// (engine.js + paint.js + the pixel ops in ui.js/dialogs.js): layers are
// real RGBA pixel buffers, composited with canvas-style blend modes; the
// brush engine stamps soft dabs along pointer strokes; history stores
// copy-on-write pixel snapshots that undo/redo genuinely restore.
//
// The core owns the whole render path: attach(app) registers custom-paint
// handlers (data-aui-paint) for the stage, the navigator, and per-layer
// thumbnails, so per-frame painting never crosses into Python. Python
// drives document *operations* and routes pointer input.
//
// Hard-to-crash contract: invalid ids/indices/points/colors no-op
// (returning false/zero/defaults); nothing here throws for bad input.

#pragma once

// NOTE: this core knows NOTHING about affineui — deliberately, and it includes
// none of its headers.
//
// It used to take an `affineui::App&`, which dragged in the whole library (and
// with it a SECOND copy of sokol_app: two `_sapp_macos_view` ObjC classes in one
// process, which macOS warns "may cause spurious casting failures and mysterious
// crashes"). A raster engine has no business knowing what a window is. It takes a
// Host of plain callbacks instead; the caller supplies them.
//
// Painting used to be the last coupling: the core drew through `affineui::Painter&`.
// Even as a pure-virtual interface that was a hard ABI coupling — the core, compiled
// into a SEPARATE module, was pinned to the runtime's exact vtable layout (reorder a
// virtual and it silently calls the wrong slot), and pybind needed
// `typeid(affineui::Painter)`, whose typeinfo lives only in the affineui runtime
// (Painter has a key function). Python loads extension modules RTLD_LOCAL, so the
// core could not borrow it and failed to import outright.
//
// So the core now declares the surface IT wants — `photo::Canvas`, below, in the
// core's own types — and the host adapts whatever painter it actually has to it.
// Nothing here is an affineui type.

#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace photo {

// RGBA8, non-premultiplied, tightly packed (stride = w * 4).
struct Buffer {
    int w = 0, h = 0;
    std::vector<std::uint8_t> px;

    Buffer() = default;
    Buffer(int width, int height)
        : w(width), h(height),
          px(static_cast<std::size_t>(width) * height * 4, 0) {}
};
using BufferPtr = std::shared_ptr<Buffer>;

struct RectI {
    int x = 0, y = 0, w = 0, h = 0;
    bool empty() const { return w <= 0 || h <= 0; }
};

// RGBA8, non-premultiplied — the core's own color type.
struct Color {
    std::uint8_t r = 0, g = 0, b = 0, a = 255;
};

/// The drawing surface the core needs, expressed entirely in the core's own
/// types. The HOST implements this over whatever painter it actually has; the
/// core never learns what that is.
///
/// Deliberately minimal — these four calls are everything the core draws.
/// Images are referenced by the opaque id the host handed back from
/// Host::create_image_rgba, so no image type crosses the boundary either.
class Canvas {
public:
    virtual ~Canvas() = default;

    virtual void draw_image(std::uint32_t image_id, const RectI& dst,
                            const RectI& src) = 0;
    virtual void fill_rect(const RectI& r, Color c) = 0;
    virtual void stroke_rect(const RectI& r, Color c, float width) = 0;
    virtual void stroke_line(float x0, float y0, float x1, float y1, Color c,
                             float width) = 0;
};

// UI-facing layer snapshot (metadata only; pixels stay in the core).
struct LayerInfo {
    int id = 0;
    std::string name;
    std::string kind = "pixel";          // "pixel" | "text"
    std::string blend = "source-over";   // canvas composite-op names
    bool visible = true;
    bool locked = false;
    double opacity = 1.0;                // 0..1
    double fill = 1.0;                   // 0..1
};

struct HistoryEntry {
    std::string name;
    std::string icon;  // decius di-* icon name
};

class PhotoDoc {
public:
    PhotoDoc(int width, int height);
    ~PhotoDoc();

    PhotoDoc(const PhotoDoc&) = delete;
    PhotoDoc& operator=(const PhotoDoc&) = delete;

    // ── Document ────────────────────────────────────────────────────────
    int width() const { return w_; }
    int height() const { return h_; }
    // Fresh single-layer document; history restarts. `background_hex` may
    // be empty for a transparent background.
    void new_document(int width, int height, const std::string& background_hex);
    // Web-parity boot scene (engine.js generateSampleScene). History
    // restarts at "Open".
    void load_sample_scene();

    // ── View transform (engine.js PS.view) ──────────────────────────────
    double zoom() const { return zoom_; }
    double pan_x() const { return pan_x_; }
    double pan_y() const { return pan_y_; }
    void set_pan(double px, double py);
    // setZoom with an optional screen-space anchor (window coords); the
    // doc point under the anchor stays put, like ctrl-wheel in the web.
    void set_zoom(double z);
    void set_zoom_at(double z, double client_x, double client_y);
    void fit_to_screen();
    // Border-box rect of the stage canvas element, window coords, as of
    // the last paint. {0,0,0,0} before the first frame.
    RectI stage_rect() const { return stage_rect_; }
    // screenToDoc: window coords → document pixel coords.
    void screen_to_doc(double client_x, double client_y,
                       double& doc_x, double& doc_y) const;
    // docOrigin: window coords of document (0,0).
    void doc_origin(double& x, double& y) const;

    // ── Layers (bottom → top, like the web layer array) ─────────────────
    std::vector<LayerInfo> layers() const;
    LayerInfo active_layer() const;
    int active_index() const { return active_; }
    int active_id() const;
    bool set_active_index(int index);
    bool set_active_id(int id);
    // Returns the new layer's id (new layers go on top and become active).
    int add_layer(const std::string& name, const std::string& blend,
                  double opacity01, const std::string& label,
                  const std::string& icon);
    bool duplicate_active();
    bool delete_active();
    bool move_active(int dir);                    // ±1 within the stack
    bool reorder_layer(int id, int new_index);    // drag-and-drop
    bool rename_layer(int id, const std::string& name);
    bool toggle_visible(int id);
    // Property setters do NOT snapshot (the web snapshots opacity/fill
    // scrubs once on pointer-up); call snapshot() explicitly.
    bool set_active_opacity(double v01);
    bool set_active_fill(double v01);
    bool set_active_blend(const std::string& blend);  // snapshots (web parity)
    bool set_active_locked(bool locked);              // snapshots
    bool merge_down();
    bool flatten();

    // ── Selection (rect marquee, like the web) ──────────────────────────
    void set_selection(int x, int y, int w, int h);
    void clear_selection();
    bool has_selection() const { return !sel_.empty(); }
    RectI selection() const { return sel_; }
    void select_all();
    bool delete_selection_pixels();  // ⌫ Clear; snapshots

    // ── Brush engine (paint.js) ─────────────────────────────────────────
    // tool: brush|pencil|eraser|clone|history|dodge|burn|smudge|blur.
    // Coordinates in document pixels. Returns false when the stroke can't
    // start (locked layer, clone without a source).
    bool begin_stroke(const std::string& tool, double x, double y,
                      double size_px, double hardness01, double opacity01,
                      double flow01, const std::string& color_hex);
    void stroke_to(double x, double y);
    void end_stroke();  // snapshots
    void cancel_stroke();
    bool stroking() const { return stroke_ != nullptr; }
    void set_clone_source(double x, double y);  // active layer at (x, y)
    bool has_clone_source() const { return clone_source_set_; }

    // ── Move tool ───────────────────────────────────────────────────────
    bool begin_move();
    void move_to(double dx, double dy);
    void end_move();  // snapshots

    // ── Click tools ─────────────────────────────────────────────────────
    bool fill_at(double x, double y, const std::string& hex,
                 double tolerance, bool contiguous, double opacity01);
    // Magic wand: select the similar-color region's bbox on the composite.
    // Returns the selection ({0,0,0,0} on miss).
    RectI wand_select(double x, double y, double tolerance, bool contiguous);
    // Eyedropper on the composite; "" when outside the document.
    std::string pick_color(double x, double y) const;
    bool apply_gradient(double x0, double y0, double x1, double y1,
                        const std::string& hex, double opacity01);
    // Type tool: rasterizes `text` into a fresh text layer at (x, y)
    // (baseline-top, like the web). Returns the layer id (0 on failure).
    int place_type(double x, double y, const std::string& text,
                   double size_px, const std::string& hex, bool bold);
    bool draw_shape(double x0, double y0, double x1, double y1,
                    const std::string& hex, double corner_radius);

    // Pen tool preview (overlay polyline drawn by the stage painter).
    void pen_add_point(double x, double y);
    void pen_clear();
    int pen_count() const { return static_cast<int>(pen_pts_.size()); }

    // ── Edit-menu ops ───────────────────────────────────────────────────
    // Fill dialog / paint-bucketless fill: whole layer (or selection).
    bool fill_layer(const std::string& hex, double opacity01,
                    const std::string& blend, const std::string& label);
    // Stroke dialog: outline the selection (or near-document rect).
    bool stroke_selection(const std::string& hex, double width,
                          const std::string& location, double opacity01);

    // ── Document geometry ───────────────────────────────────────────────
    bool apply_crop();  // crop to the current selection
    bool crop_to(int x, int y, int w, int h);
    bool resize_image(int w, int h);                       // bilinear scale
    bool resize_canvas(int w, int h, const std::string& anchor);  // tl..br

    // ── Adjustments & filters ───────────────────────────────────────────
    // Modal live preview: begin captures the active layer's region; each
    // preview_adjust re-applies onto the captured base; commit snapshots,
    // cancel restores. kinds: bc(b,c) hsl(h,s,l) levels(lo,hi,gamma100)
    // vibrance(a) generic(a) blur(radius) noise(a).
    bool begin_preview();
    void preview_adjust(const std::string& kind, double a, double b,
                        double c);
    void commit_preview(const std::string& label, const std::string& icon);
    void cancel_preview();
    bool previewing() const { return preview_ != nullptr; }
    // One-shot ops (no modal): invert desat threshold sharpen emboss
    // findedges noise pixelate. Snapshots with label/icon.
    bool apply_adjust(const std::string& kind, double a,
                      const std::string& label, const std::string& icon);

    // ── Place Embedded assets (dialogs.js dlgPlace) ─────────────────────
    // asset: "Sun flare" | "Gradient map" | "Noise texture" | "Vignette".
    int place_asset(const std::string& asset);

    // ── History (pixel snapshots, COW-shared buffers) ───────────────────
    std::vector<HistoryEntry> history_entries() const;
    int history_index() const { return history_index_; }
    /// Stable id of the state currently shown, or 0 when there is no history.
    /// Compare THIS across time, not history_index(): indices are reused when a
    /// snapshot after an undo erases the redo tail, so the same index can mean
    /// different pixels. An app answering "are there unsaved changes?" records
    /// the id at save and compares it later.
    std::uint64_t history_id() const {
        return (history_index_ >= 0 &&
                history_index_ < static_cast<int>(history_.size()))
                   ? history_[static_cast<std::size_t>(history_index_)].id
                   : 0;
    }
    int history_source() const { return history_source_; }
    void set_history_source(int index);
    void snapshot(const std::string& name, const std::string& icon);
    std::string undo();
    std::string redo();
    bool jump_to(int index);

    // ── Export ──────────────────────────────────────────────────────────
    // Flattened composite as PNG. scale ∈ {0.5, 1, 2}. `opaque_white`
    // fills the backdrop (JPG-style exports in the web).
    bool export_png(const std::string& path, double scale,
                    bool opaque_white) const;

    // ── UI integration ──────────────────────────────────────────────────
    // Everything the core needs from whatever is hosting it. No App, no window,
    // no sokol — just callbacks. The host wires these to its own app (the
    // Python binding builds them from an affineui.App).
    //
    // Image ids are opaque to the core: 0 means "none/failed". Pixel spans are
    // borrowed for the duration of the call — the host must not retain them.
    struct Host {
        // The host hands the core a Canvas it implements itself, plus the
        // element rect to draw into. Both are the core's own types.
        using PaintFn = std::function<void(Canvas&, const RectI&)>;
        using Pixels = std::span<const std::uint8_t>;

        // Register (or, with a null fn, unregister) a custom-paint handler.
        std::function<void(std::string_view name, PaintFn fn)> set_custom_paint;
        // Mark every element bound to `name` for repaint next frame.
        std::function<void(std::string_view name)> request_custom_repaint;
        // Dynamic RGBA images. create returns 0 on failure.
        std::function<std::uint32_t(int w, int h, Pixels px)> create_image_rgba;
        std::function<bool(std::uint32_t id, Pixels px)>      update_image;
        std::function<void(std::uint32_t id)>                 destroy_image;
        // Bytes of the embedded UI font, for glyph rasterization (the type
        // tool). Empty span is tolerated — text just won't render.
        std::function<std::string_view(bool bold)> font_data;

        // A host is usable once it can register paint handlers.
        explicit operator bool() const noexcept {
            return static_cast<bool>(set_custom_paint);
        }
    };

    // Registers the custom-paint handlers through the host:
    //   "ps-stage"          document composite w/ zoom+pan + pen preview
    //   "ps-nav"            navigator thumbnail
    //   "ps-thumb-<id>"     per-layer thumbnails (kept in sync with the
    //                       layer stack; stale names are unregistered)
    void attach(Host host);
    void detach();

private:
    // Hand an image back to the host and zero the id. No-op on 0 / detached.
    void destroy_image(std::uint32_t& id);

public:
    // Mark the stage + navigator (+ dirty thumbs) for repaint next frame.
    void request_repaint();
    std::string thumb_paint_name(int layer_id) const;

    // Monotonic revision that bumps on any visible pixel/stack change —
    // the cheap "did anything change?" probe for the Python shell.
    std::uint64_t revision() const { return revision_; }

private:
    struct Layer {
        int id = 0;
        std::string name;
        std::string kind = "pixel";
        std::string blend = "source-over";
        bool visible = true;
        bool locked = false;
        double opacity = 1.0;
        double fill = 1.0;
        BufferPtr pixels;
        std::uint64_t pixel_rev = 0;  // bumps when pixels change
    };

    struct LayerRec {  // history record entry (shares pixel buffers)
        int id;
        std::string name, kind, blend;
        bool visible, locked;
        double opacity, fill;
        BufferPtr pixels;
    };
    struct HistoryRec {
        std::string name, icon;
        int active = 0;
        int w = 0, h = 0;
        std::vector<LayerRec> layers;
        // Stable identity for this state. History INDICES are reused across
        // branches — a snapshot taken after an undo erases the redo tail and
        // lands back on the same index with different pixels — so an index is
        // not something a caller can compare against to answer "is this still
        // the state I saved?". This is.
        std::uint64_t id = 0;
    };

    struct StrokeState;   // brush engine scratch (photo_core.cpp)
    struct MoveState;
    struct PreviewState;

    Layer* active_ptr();
    const Layer* active_ptr() const;
    Layer* find_layer(int id);
    Layer* unlocked_active();
    // Clone-on-write: make the layer's buffer unique before mutating.
    Buffer& writable(Layer& layer);
    void mark_layer_dirty(Layer& layer, const RectI& rect);
    void mark_all_dirty();

    // Compositing (engine.js PS.render): rebuild composite_ (pure,
    // transparent backdrop — the wand/eyedropper source) and display_
    // (checkerboard underlay + composite) inside `rect`.
    void composite_rect(const RectI& rect);
    void flush_composite();  // apply pending dirty region, if any

    RectI doc_rect() const { return {0, 0, w_, h_}; }
    RectI sel_or_doc() const { return sel_.empty() ? doc_rect() : sel_; }

    void paint_stage(Canvas& p, const RectI& r);
    void paint_nav(Canvas& p, const RectI& r);
    void paint_thumb(int layer_id, Canvas& p, const RectI& r);
    void sync_thumb_handlers();  // register/unregister per-layer thumbs

    // Lifetime token for the paint lambdas: they outlive nothing, but they are
    // held by the host, so a destroyed doc must not be called back into. (This
    // replaces affineui::Trackable — the core owns its own lifetime story.)
    std::shared_ptr<PhotoDoc*> alive_ = std::make_shared<PhotoDoc*>(this);

    void reset_history(const std::string& label, const std::string& icon);
    void restore(const HistoryRec& rec);

    int next_id_ = 1;
    int w_ = 0, h_ = 0;
    std::vector<Layer> layers_;
    int active_ = -1;
    RectI sel_{};

    double zoom_ = 0.67, pan_x_ = 0.0, pan_y_ = 0.0;
    bool need_fit_ = true;
    RectI stage_rect_{};

    Buffer composite_;   // layers only, transparent backdrop
    Buffer display_;     // checker underlay + composite (what the GPU sees)
    RectI pending_dirty_{};
    bool full_dirty_ = true;

    std::vector<HistoryRec> history_;
    int history_index_ = -1;
    // Monotonic; never reused, so a saved id stays meaningful across branches.
    std::uint64_t history_next_id_ = 0;
    int history_source_ = 0;
    static constexpr std::size_t kHistoryMax = 40;  // web cap

    std::unique_ptr<StrokeState> stroke_;
    std::unique_ptr<MoveState> move_;
    std::unique_ptr<PreviewState> preview_;
    bool clone_source_set_ = false;
    double clone_x_ = 0.0, clone_y_ = 0.0;
    int clone_layer_id_ = 0;

    struct PenPt { double x, y; };
    std::vector<PenPt> pen_pts_;

    // GPU side (valid only while attached; images live in the host's painter
    // and are refreshed lazily against *_rev counters). Ids are opaque; 0 =
    // none. The host owns their lifetime — we hand them back via
    // Host::destroy_image.
    Host host_{};
    std::uint32_t stage_img_ = 0;
    int stage_img_w_ = 0, stage_img_h_ = 0;
    std::uint64_t uploaded_rev_ = 0;
    std::uint64_t revision_ = 1;
    struct ThumbTex {
        std::uint32_t img = 0;
        std::uint64_t rev = 0;   // layer pixel_rev the texture reflects
    };
    std::unordered_map<int, ThumbTex> thumbs_;      // by layer id
    std::unordered_set<int> thumb_names_;           // registered handlers
};

}  // namespace photo
