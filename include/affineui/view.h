#pragma once

// Remote-ready command tree for AffineUI applications.
//
// This is intentionally not a second renderer. The tree is an identity and
// reconciliation spine for app-level widget commands. Local native mode stores
// weak handles to real AffineUI DOM nodes; browser mode stores stable remote ids
// and emits DOM patch operations. The public command API can feel immediate,
// while the backing output stays retained and efficient.

#include <cstdint>
#include <functional>
#include <optional>
#include <source_location>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "affineui/callback.h"
#include "affineui/document.h"  // Document::DockPlacement (dock override readback)
#include "affineui/types.h"

namespace affineui {

struct StableId {
    std::uint64_t value{0};

    friend bool operator==(StableId a, StableId b) noexcept {
        return a.value == b.value;
    }
    friend bool operator!=(StableId a, StableId b) noexcept {
        return !(a == b);
    }
};

enum class ViewTheme {
    Plain,
    Bootstrap,
    Decius,
};

/// A parsed, comparable CSS-framework version (major.minor.patch). Used to
/// branch markup/behavior across framework versions — a personality module
/// (or the interaction layer) parses the active `data-aui-framework-version`
/// and compares it, e.g.
/// `if (FrameworkVersion::parse(v) >= FrameworkVersion{0,6,0}) { ... }`.
/// (Named FrameworkVersion to distinguish from the library's own Version in
/// version.h.) Parsing is lenient: missing components are 0, trailing junk is
/// ignored.
struct FrameworkVersion {
    int major{0};
    int minor{0};
    int patch{0};

    [[nodiscard]] static FrameworkVersion parse(std::string_view text) noexcept;

    friend bool operator==(const FrameworkVersion& a, const FrameworkVersion& b) noexcept {
        return a.major == b.major && a.minor == b.minor && a.patch == b.patch;
    }
    friend bool operator<(const FrameworkVersion& a, const FrameworkVersion& b) noexcept {
        if (a.major != b.major) return a.major < b.major;
        if (a.minor != b.minor) return a.minor < b.minor;
        return a.patch < b.patch;
    }
    friend bool operator!=(const FrameworkVersion& a, const FrameworkVersion& b) noexcept { return !(a == b); }
    friend bool operator>(const FrameworkVersion& a, const FrameworkVersion& b) noexcept { return b < a; }
    friend bool operator<=(const FrameworkVersion& a, const FrameworkVersion& b) noexcept { return !(b < a); }
    friend bool operator>=(const FrameworkVersion& a, const FrameworkVersion& b) noexcept { return !(a < b); }
};

namespace bootstrap {
/// The Bootstrap version AffineUI ships with / targets by default.
inline constexpr std::string_view default_version{"5.3.8"};

namespace selector {
inline constexpr std::string_view size{"size"};
inline constexpr std::string_view theme{"theme"};
}  // namespace selector

namespace size {
inline constexpr std::string_view sm{"sm"};
inline constexpr std::string_view med{"md"};
inline constexpr std::string_view medium{"md"};
inline constexpr std::string_view md{"md"};
inline constexpr std::string_view lg{"lg"};
}  // namespace size

namespace theme {
inline constexpr std::string_view light{"light"};
inline constexpr std::string_view dark{"dark"};
}  // namespace theme

namespace class_name {
inline constexpr std::string_view form{"aui-bs-form"};
inline constexpr std::string_view props{"aui-bs-props"};
inline constexpr std::string_view column{"aui-bs-col"};
inline constexpr std::string_view row{"aui-bs-row"};
inline constexpr std::string_view note{"form-text"};
inline constexpr std::string_view button_row{"aui-bs-btn-row"};
inline constexpr std::string_view list{"list-group"};
inline constexpr std::string_view list_item{"list-group-item"};
inline constexpr std::string_view tree{"list-group"};
inline constexpr std::string_view tree_row{"list-group-item"};
inline constexpr std::string_view table{"table"};
}  // namespace class_name
}  // namespace bootstrap

namespace decius {
/// The Decius CSS bundle version AffineUI ships with and targets by default.
/// An app may declare a different version via View::set_decius_version(); the
/// stylesheet href derives from it. Declaring the version you shipped and
/// tested against is recommended (pin it like a dependency).
inline constexpr std::string_view default_version{"0.6.2"};

/// The bundle stylesheet href for a given Decius version, e.g.
/// "frameworks/css/decius-css-0.5.2.bundle.min.css".
[[nodiscard]] std::string bundle_href(std::string_view version);

namespace selector {
inline constexpr std::string_view size{"size"};
inline constexpr std::string_view style{"style"};
inline constexpr std::string_view density{"density"};
inline constexpr std::string_view accent{"accent"};
inline constexpr std::string_view radius{"radius"};
inline constexpr std::string_view dark{"dark"};
}  // namespace selector

namespace size {
inline constexpr std::string_view sm{"sm"};
inline constexpr std::string_view med{"md"};
inline constexpr std::string_view medium{"md"};
inline constexpr std::string_view md{"md"};
inline constexpr std::string_view lg{"lg"};
}  // namespace size

namespace style {
inline constexpr std::string_view flat{"flat"};
inline constexpr std::string_view three_d{"3d"};
}  // namespace style

namespace density {
inline constexpr std::string_view compact{"compact"};
inline constexpr std::string_view comfortable{"comfortable"};
inline constexpr std::string_view spacious{"spacious"};
}  // namespace density

namespace class_name {
inline constexpr std::string_view form{"dcs-form"};
inline constexpr std::string_view props{"dcs-props"};
inline constexpr std::string_view column{"dcs-col"};
inline constexpr std::string_view row{"dcs-row"};
inline constexpr std::string_view note{"dcs-note"};
inline constexpr std::string_view button_row{"dcs-btn-row"};
inline constexpr std::string_view list{"dcs-list"};
inline constexpr std::string_view list_item{"dcs-list__item"};
inline constexpr std::string_view tree{"dcs-tree"};
inline constexpr std::string_view tree_row{"dcs-tree__row"};
inline constexpr std::string_view table{"dcs-table"};
}  // namespace class_name
}  // namespace decius

/// The CSS bundle stylesheet href a theme would request at a given version
/// (empty version = the theme's default). Empty for the Plain theme. Exposed so
/// app code can load the bundle from disk itself (see the app template's
/// styles loader) rather than relying on <link> resolution.
[[nodiscard]] std::string framework_bundle_href(ViewTheme theme,
                                                std::string_view version = {});

/// The default framework version for a theme ("" for Plain).
[[nodiscard]] std::string_view framework_default_version(ViewTheme theme);

enum class WidgetKind {
    Root,
    Container,
    Text,
    RawHtml,
    Heading,
    Panel,
    Button,
    Checkbox,
    Slider,
    Knob,
    TextInput,
    TextArea,
    Dropdown,
    ButtonGroup,
    VirtualList,
    Card,
};

struct VirtualListOptions {
    std::size_t item_count{0};
    std::size_t first_item{0};
    std::size_t visible_items{16};
    std::size_t overscan{2};
    double item_size{24.0};
    std::vector<double> item_sizes;
};

/// A floating tool rail's configuration. `drag_bounds` is a CSS selector for
/// the region it can be dragged within (e.g. ".viewport-canvas"); `position`
/// is its initial inline placement (e.g. "left:8px;top:8px").
struct FloatingToolbarOptions {
    bool             vertical{true};   // stack icons vertically (else a row)
    bool             small{true};       // the compact --sm size
    std::string_view drag_bounds;      // data-dcs-drag-bounds selector
    std::string_view position;         // inline left/top placement
};

/// Where a dockable panel docks relative to its parent. Tab adds it as another
/// tab of the parent's pane.
enum class Dock { Left, Right, Top, Bottom, Tab };

/// A dockable's window state.
enum class DockState { Docked, Detached /*floating OS window*/, Tearoff };

/// A corner of the parent that a floating panel is anchored to.
enum class DockCorner { TopLeft, TopRight, BottomLeft, BottomRight };

class View;  // for DockHandle::toolbar (defined out-of-line in view.cpp)

/// A handle to a declared dockable — usable as another dockable's parent and
/// as the target of the container's runtime add/remove API.
struct DockHandle {
    std::string id;
    [[nodiscard]] explicit operator bool() const noexcept { return !id.empty(); }

    /// Declare this pane's tab toolbar (the strip beside the tabs — filter
    /// buttons, a search field, a viewport's mode/tool controls). Optional;
    /// call it right after document()/dockpanel(). `build` fills the toolbar.
    /// Returns *this so it chains. A no-op on a handle with no owning view.
    DockHandle& toolbar(const std::function<void(View&)>& build);

private:
    friend class View;
    View* owner_{nullptr};  // set by document()/dockpanel() so toolbar() can
                            // reach back into the live dock recorder
};

/// Everything about where a dockable is (or starts) — one value type passed to
/// dockpanel(), rather than a combinatorial set of overloads. All fields are
/// optional; only what's relevant to the chosen state need be set. Build them
/// with the factory helpers (docked(), tab(), floating(), tearoff()) and chain
/// the `.in()/.sized()/.dragging_with()` setters.
struct DockLocation {
    std::optional<Dock>        side;        // docked side / Tab (when Docked)
    std::optional<std::string> parent;      // parent dockable id (none = the document)
    DockState                  state{DockState::Docked};
    std::optional<int>         size;        // px flex-basis when docked
    std::optional<DockCorner>  anchor;      // corner a float is anchored to
    std::optional<std::pair<int, int>> offset;      // float pos relative to anchor (px)
    std::optional<std::pair<int, int>> float_size;  // float size (w,h px)
    std::optional<std::string> drag_with;   // tearoff: id of the panel it drags with

    DockLocation() = default;
    // Implicit: `Dock::Left` reads as "docked Left of the document".
    DockLocation(Dock s) : side(s) {}  // NOLINT(google-explicit-constructor)

    // Fluent setters (chainable).
    DockLocation& in(DockHandle parent_panel) { parent = parent_panel.id; return *this; }
    DockLocation& sized(int px) { size = px; return *this; }
    DockLocation& tearout_size(std::pair<int, int> px) { float_size = px; return *this; }
    DockLocation& dragging_with(DockHandle p) { drag_with = p.id; return *this; }

    // Factories for the common shapes.
    static DockLocation docked(Dock s, int px = 0) {
        DockLocation l; l.side = s; if (px) l.size = px; return l;
    }
    static DockLocation tab() {
        DockLocation l; l.side = Dock::Tab; return l;
    }
    static DockLocation floating(DockCorner anchor, std::pair<int, int> pos,
                                 std::pair<int, int> size) {
        DockLocation l; l.state = DockState::Detached; l.anchor = anchor;
        l.offset = pos; l.float_size = size; return l;
    }
    static DockLocation tearoff(DockCorner anchor, std::pair<int, int> pos,
                                std::pair<int, int> size) {
        DockLocation l = floating(anchor, pos, size);
        l.state = DockState::Tearoff; return l;
    }
};

/// Presentation for a tree row. The row component generates the canonical
/// Decius substructure — a chevron (open/closed/leaf-placeholder), a type
/// icon, the label, and an optional trailing meta icon — so callers declare
/// intent, not markup. `icon` / `meta_icon` are Decius icon-font glyph names
/// ("cube", "eye", …); empty omits that slot. `depth` indents the row
/// (flat-list model: rows are siblings, --depth gives the visual nesting).
struct TreeRowOptions {
    int              depth{0};
    bool             selected{false};
    std::string_view icon;            // leading type-icon glyph (di-<icon>)
    bool             expandable{false};  // row can expand → show a chevron
    bool             expanded{true};      // chevron open (children visible)
    bool             draggable{true};
    std::string_view meta_icon;      // trailing meta glyph, e.g. "eye"
};

struct WidgetAttribute {
    std::string name;
    std::string value;
};

struct WidgetNode {
    StableId id{};
    WidgetKind kind{WidgetKind::Container};
    std::string tag;
    std::string widget_name;
    std::string remote_id;
    DomHandle local_dom{};

    // Component provenance: the builder call site that created this node
    // (affinetools `view` domain — "which C++ call made this DOM node").
    // file_name() points at a static string literal, so retaining the
    // pointer is safe and costs no allocation.
    const char*   src_file{nullptr};
    std::uint32_t src_line{0};

    std::string text;
    std::vector<WidgetAttribute> attrs;
    std::vector<WidgetNode> children;

    // Reconcile cursor, reset at the start of each visit. Public so debug
    // inspectors can show the machinery plainly; user code should treat it as
    // internal state.
    std::size_t cursor{0};

    // End-of-build attribute coalescing (WIDGET_RECONCILIATION.md §5.2):
    // attribute writes inside a build pass mutate `attrs` without emitting;
    // View::end() diffs against `attrs_before` (snapshot at first mutation
    // this pass) and emits only the net change, so declare-then-modify
    // patterns (base classes, then a `.cls()` modifier) reconcile to zero
    // ops when the end-of-build value is unchanged. Multiple `style` writes
    // within one pass compose by declaration merge instead of replacing.
    // Internal state; empty/false outside a pass.
    std::vector<WidgetAttribute> attrs_before;
    bool attrs_touched{false};
    bool style_written{false};
};

struct WidgetClickBinding {
    std::string name;
    std::function<void()> handler;
};

struct WidgetChangeBinding {
    std::string name;
    std::function<void(std::string_view)> handler;
};

enum class RemotePatchOp {
    CreateElement,
    CreateText,
    Remove,
    SetText,
    SetAttribute,
    RemoveAttribute,
};

struct RemotePatch {
    RemotePatchOp op{RemotePatchOp::CreateElement};
    std::string id;
    std::string parent_id;
    std::string tag;
    std::string name;
    std::string value;
    std::size_t index{0};
};

class RemotePatchQueue {
public:
    void clear();
    void push(RemotePatch patch);

    [[nodiscard]] bool empty() const noexcept { return patches_.empty(); }
    [[nodiscard]] std::size_t size() const noexcept { return patches_.size(); }
    [[nodiscard]] const std::vector<RemotePatch>& patches() const noexcept {
        return patches_;
    }

    [[nodiscard]] std::string to_json() const;

private:
    std::vector<RemotePatch> patches_;
};

class ViewSink {
public:
    virtual ~ViewSink() = default;

    virtual void create_element(const WidgetNode& node,
                                const WidgetNode* parent,
                                std::size_t index) = 0;
    virtual void create_text(const WidgetNode& node,
                             const WidgetNode* parent,
                             std::size_t index) = 0;
    virtual void remove(const WidgetNode& node) = 0;
    virtual void set_text(const WidgetNode& node, std::string_view value) = 0;
    virtual void set_attribute(const WidgetNode& node,
                               std::string_view name,
                               std::string_view value) = 0;
    virtual void remove_attribute(const WidgetNode& node,
                                  std::string_view name) = 0;

    /// Raw-HTML nodes (View::html). `create_raw_html` fires when the
    /// node enters the tree (its markup arrives in the set_raw_html
    /// that immediately follows); `set_raw_html` fires whenever the
    /// markup string changes. Default no-ops so sinks that cannot
    /// represent raw HTML (remote patch streams) simply skip them —
    /// the local document sink parses the fragment in place.
    virtual void create_raw_html(const WidgetNode& node,
                                 const WidgetNode* parent,
                                 std::size_t index) {
        (void)node; (void)parent; (void)index;
    }
    virtual void set_raw_html(const WidgetNode& node,
                              std::string_view markup) {
        (void)node; (void)markup;
    }
};

class RemotePatchSink final : public ViewSink {
public:
    explicit RemotePatchSink(RemotePatchQueue* queue = nullptr) noexcept
        : queue_(queue) {}

    void reset(RemotePatchQueue* queue) noexcept { queue_ = queue; }

    void create_element(const WidgetNode& node,
                        const WidgetNode* parent,
                        std::size_t index) override;
    void create_text(const WidgetNode& node,
                     const WidgetNode* parent,
                     std::size_t index) override;
    void remove(const WidgetNode& node) override;
    void set_text(const WidgetNode& node, std::string_view value) override;
    void set_attribute(const WidgetNode& node,
                       std::string_view name,
                       std::string_view value) override;
    void remove_attribute(const WidgetNode& node,
                          std::string_view name) override;

private:
    RemotePatchQueue* queue_{nullptr};
};

class View;

/// Validity of a strongly-typed component wrapper (see components.h). Lives
/// here because View::component<T>() produces it and components.h depends on
/// View. Three states: a wrong-type query stays *attached* to the node it
/// found (so you can introspect it) but is not Valid.
enum class ComponentValidity {
    Valid,       ///< Bound to a live node of the expected type.
    WrongType,   ///< Bound to a live node, but NOT this component's type.
    NotPresent,  ///< No node with that id resolves (unbound or removed).
};

class WidgetRef {
public:
    WidgetRef() noexcept = default;
    WidgetRef(View* owner,
              StableId panel_id,
              StableId id,
              std::string_view name = {});
    WidgetRef(const WidgetRef&) = default;
    WidgetRef& operator=(const WidgetRef&) = default;
    WidgetRef(WidgetRef&&) noexcept = default;
    WidgetRef& operator=(WidgetRef&&) noexcept = default;
    ~WidgetRef() = default;

    [[nodiscard]] explicit operator bool() const;
    [[nodiscard]] StableId id() const;
    [[nodiscard]] const WidgetNode* node() const;
    [[nodiscard]] std::string_view name() const;

    // Reads (for strongly-typed component wrappers). All degrade gracefully:
    // if the referenced node is gone they return the supplied/empty default
    // rather than crash, mirroring the no-op behaviour of the mutators.
    [[nodiscard]] std::string_view attr_value(std::string_view name,
                                              std::string_view fallback = {}) const;
    [[nodiscard]] std::string_view text_value() const;
    [[nodiscard]] bool has_attr(std::string_view name) const;

    WidgetRef& named(std::string_view name);
    WidgetRef& clear();
    WidgetRef& text(std::string_view value);
    WidgetRef& attr(std::string_view name, std::string_view value);
    WidgetRef& remove_attr(std::string_view name);
    WidgetRef& selector(std::string_view name, std::string_view value);
    /// Replace the element's class list. NOTE: this overwrites whatever the
    /// widget builder set (e.g. a Decius button's `dcs-btn`). To ADD a
    /// modifier while keeping the framework's base classes, use add_class().
    WidgetRef& cls(std::string_view classes);
    /// Append a class token (idempotent) without disturbing the classes the
    /// widget builder already applied — the right way to add a variant like
    /// `dcs-btn--sm` to a `button()` or an app-specific hook to any widget.
    WidgetRef& add_class(std::string_view token);
    WidgetRef& on_click(std::function<void()> cb);
    WidgetRef& on_change(std::function<void(std::string_view)> cb);
    /// Fires only on COMMITTED changes: the end of a continuous gesture
    /// (a combo/slider/knob scrub, a colour-picker drag) or a discrete
    /// edit. on_change streams every change including the live ones —
    /// bind cheap preview work there and the undoable command here, so
    /// a scrub previews continuously and commits once.
    WidgetRef& on_commit(std::function<void(std::string_view)> cb);
    WidgetRef& append(const std::function<void(View&)>& build);
    WidgetRef& replace(const std::function<void(View&)>& build);

    [[nodiscard]] WidgetRef find_widget(std::string_view name) const;

private:
    View* owner_{nullptr};
    StableId panel_id_{};
    mutable StableId id_{};
    std::string name_;

    friend class View;
};

class View {
public:
    class Scope {
    public:
        Scope() noexcept = default;
        // `unwind_to` is the stack depth this scope restores on destruction —
        // i.e. the depth before its node(s) were opened. Multi-level builders
        // (e.g. dock_panel, which opens pane > body) record the pre-open depth
        // so the whole sub-tree closes as one RAII unit.
        Scope(View* owner, WidgetNode* node, std::size_t unwind_to) noexcept;
        ~Scope();

        Scope(Scope&& other) noexcept;
        Scope& operator=(Scope&& other) noexcept;
        Scope(const Scope&) = delete;
        Scope& operator=(const Scope&) = delete;

        explicit operator bool() const noexcept { return owner_ != nullptr; }

        WidgetRef ref() const;
        Scope& named(std::string_view name);
        Scope& attr(std::string_view name, std::string_view value);
        Scope& selector(std::string_view name, std::string_view value);
        Scope& cls(std::string_view classes);
        Scope& text(std::string_view value);
        [[nodiscard]] WidgetRef find_widget(std::string_view name) const;

    private:
        View*       owner_{nullptr};
        WidgetNode* node_{nullptr};
        std::size_t unwind_to_{0};
    };

    // Decius is the default theme — it's the framework the engine ships
    // its embedded CSS + icon font for (see include/affineui/decius_bundle.h),
    // so `View v;` + `App(Config{}).load_view(v)` produces a styled UI
    // with zero explicit configuration. Pass `ViewTheme::Bootstrap` etc.
    // to opt into a different framework's class-name style.
    explicit View(ViewTheme theme = ViewTheme::Decius);

    void clear();
    View& selector(std::string_view name, std::string_view value);
    void set_mutation_sink(ViewSink* sink) noexcept { mutation_sink_ = sink; }
    void begin(ViewSink* sink = nullptr);
    void begin(RemotePatchQueue* remote_patches);
    void end();
    [[nodiscard]] const std::vector<std::string>& diagnostics() const noexcept {
        return diagnostics_;
    }
    void clear_diagnostics() { diagnostics_.clear(); }

    [[nodiscard]] ViewTheme theme() const noexcept { return theme_; }
    void set_theme(ViewTheme theme) noexcept { theme_ = theme; }

    /// Declare the CSS framework version this view targets (e.g. "0.5.2" for
    /// Decius, "5.3.8" for Bootstrap). The stylesheet href derives from it and
    /// it is stamped on the document root as data-aui-framework-version so the
    /// interaction layer / version-branching personalities can read it. Empty
    /// (default) means "use the personality's default version".
    void set_framework_version(std::string_view version) {
        framework_version_ = std::string(version);
    }
    [[nodiscard]] std::string_view framework_version() const noexcept {
        return framework_version_;
    }
    [[nodiscard]] Color background_color() const noexcept;

    [[nodiscard]] const WidgetNode& root() const noexcept { return root_; }

    Scope container(std::string_view classes = {},
                    std::string_view key = {},
                    std::source_location here = std::source_location::current());
    Scope element(std::string_view tag,
                  std::string_view classes = {},
                  std::string_view key = {},
                  std::source_location here = std::source_location::current());
    Scope panel(std::string_view key = {},
                std::source_location here = std::source_location::current());
    Scope card(std::string_view title,
               std::string_view classes = {},
               std::string_view key = {},
               std::source_location here = std::source_location::current());

    WidgetRef heading(int level,
                      std::string_view text,
                      std::string_view classes = {},
                      std::string_view key = {},
                      std::source_location here = std::source_location::current());
    WidgetRef paragraph(std::string_view text,
                        std::string_view classes = {},
                        std::string_view key = {},
                        std::source_location here = std::source_location::current());
    WidgetRef text(std::string_view text,
                   std::string_view key = {},
                   std::source_location here = std::source_location::current());
    WidgetRef html(std::string_view markup,
                   std::string_view key = {},
                   std::source_location here = std::source_location::current());
    WidgetRef button(std::string_view label,
                     bool primary = false,
                     std::string_view key = {},
                     std::source_location here = std::source_location::current());
    WidgetRef checkbox(std::string_view label,
                       bool checked,
                       std::string_view key = {},
                       std::source_location here = std::source_location::current());
    /// An on/off switch (Decius `.dcs-switch`). Same semantics as checkbox —
    /// the core interaction layer toggles its aria-checked — just a sliding
    /// switch instead of a tick box. Falls back to a checkbox off-Decius.
    WidgetRef toggle(std::string_view label,
                     bool on,
                     std::string_view key = {},
                     std::source_location here = std::source_location::current());
    WidgetRef input(std::string_view label,
                    std::string_view value,
                    std::string_view type = "text",
                    std::string_view key = {},
                    std::source_location here = std::source_location::current());
    WidgetRef password(std::string_view label,
                       std::string_view value,
                       std::string_view key = {},
                       std::source_location here = std::source_location::current());
    WidgetRef textarea(std::string_view label,
                       std::string_view value,
                       int rows = 3,
                       std::string_view key = {},
                       std::source_location here = std::source_location::current());
    WidgetRef dropdown(std::string_view label,
                       const std::vector<std::string>& options,
                       std::string_view selected,
                       std::string_view key = {},
                       std::source_location here = std::source_location::current());
    WidgetRef button_group(std::string_view label,
                           const std::vector<std::string>& options,
                           std::string_view selected,
                           std::string_view key = {},
                           std::source_location here = std::source_location::current());
    WidgetRef virtual_list(std::string_view key,
                           const VirtualListOptions& options,
                           const std::function<void(View&, std::size_t)>& build_item,
                           std::string_view classes = {},
                           std::source_location here = std::source_location::current());
    WidgetRef slider(std::string_view label,
                     double value,
                     double min = 0.0,
                     double max = 1.0,
                     std::string_view key = {},
                     std::source_location here = std::source_location::current());
    WidgetRef knob(std::string_view label,
                   double value,
                   double min = 0.0,
                   double max = 1.0,
                   bool bipolar = false,
                   std::string_view key = {},
                   std::source_location here = std::source_location::current());
    WidgetRef container_ref(std::string_view classes = {},
                            std::string_view key = {},
                            std::source_location here = std::source_location::current());
    WidgetRef element_ref(std::string_view tag,
                          std::string_view classes = {},
                          std::string_view key = {},
                          std::source_location here = std::source_location::current());
    /// Custom paint (canvas) surface: a leaf block whose content is
    /// drawn by the app handler registered under `paint_name`
    /// (App::set_custom_paint / Document::set_custom_paint). Size and
    /// place it with classes/inline style like any element; per-frame
    /// geometry then flows through App::request_custom_repaint without
    /// touching the DOM. Emits `<div data-aui-paint="paint_name">`.
    WidgetRef canvas(std::string_view paint_name,
                     std::string_view classes = {},
                     std::string_view key = {},
                     std::source_location here = std::source_location::current());
    WidgetRef panel_ref(std::string_view key = {},
                        std::source_location here = std::source_location::current());

    // ── App-shell / structural components ───────────────────────────────
    // These aggregate themed HTML (and the data-dcs-* hooks the interaction
    // layer reads) into the compound widgets a DCC tool is built from. Each
    // returns a Scope (open container you fill with a build body) or a
    // WidgetRef you can adopt into a strongly-typed component (components.h).

    /// A toolbar row. Fill it with icon_button()/buttons/separators.
    Scope toolbar(std::string_view key = {},
                  std::source_location here = std::source_location::current());

    /// A floating, draggable toolbar (a tool rail). Emits the grip drag handle
    /// and floats over its host; `drag_bounds` is a CSS selector for the region
    /// it can be dragged within (typically a data-dcs-float-host). Fill it with
    /// icon_button()s. Returns a Scope.
    Scope floating_toolbar(const FloatingToolbarOptions& opts = {},
                           std::string_view key = {},
                           std::source_location here = std::source_location::current());
    /// A thin separator inside a toolbar.
    WidgetRef toolbar_separator(std::string_view key = {},
                                std::source_location here = std::source_location::current());
    /// An icon-only ghost button (icon = Decius icon name, e.g. "save").
    WidgetRef icon_button(std::string_view icon,
                          std::string_view key = {},
                          std::source_location here = std::source_location::current());

    /// A menubar row. Fill it with menu_button()s that target menus.
    Scope menu_bar(std::string_view key = {},
                   std::source_location here = std::source_location::current());
    /// A menubar button that opens the menu with id `menu_id` on click.
    WidgetRef menu_button(std::string_view label,
                          std::string_view menu_id,
                          std::string_view key = {},
                          std::source_location here = std::source_location::current());
    /// A menubar button that OWNS its dropdown, declared inline: the `build`
    /// creator populates the menu (menu_item/submenu/...). The menu is emitted
    /// and linked automatically — no separate id wrangling.
    WidgetRef menu_button(std::string_view label,
                          const std::function<void(View&)>& build,
                          std::string_view key = {},
                          std::source_location here = std::source_location::current());
    /// A popup menu (hidden until a menu_button / data-dcs-toggle targets its
    /// `id`). The `build` creator populates it with menu_item()/menu_separator()
    /// /submenu(). Nested menus are declared by calling submenu() inside build.
    WidgetRef menu(std::string_view id,
                   const std::function<void(View&)>& build,
                   std::source_location here = std::source_location::current());
    /// A menu row: optional leading icon glyph + label + optional shortcut.
    /// Wire on_click for the action.
    WidgetRef menu_item(std::string_view label,
                        std::string_view icon = {},
                        std::string_view shortcut = {},
                        std::string_view key = {},
                        std::source_location here = std::source_location::current());
    /// A menu row whose content the caller composes inside the returned
    /// scope (color swatches, custom layouts, ...). Activation behaves like
    /// menu_item(): hover highlight, click selects and closes the menu, and
    /// `.ref().on_click(...)` fires.
    Scope menu_item_custom(std::string_view key = {},
                           std::source_location here = std::source_location::current());
    /// A separator line between menu groups.
    WidgetRef menu_separator(std::string_view key = {},
                             std::source_location here = std::source_location::current());
    /// A submenu: a menu row with a caret that reveals its nested items (the
    /// `build` creator) on hover. Nestable to any depth.
    WidgetRef submenu(std::string_view label,
                      const std::function<void(View&)>& build,
                      std::string_view icon = {},
                      std::string_view key = {},
                      std::source_location here = std::source_location::current());

    /// The app brand at the start of a menubar: an icon glyph + title.
    WidgetRef menu_brand(std::string_view title,
                         std::string_view icon = {},
                         std::string_view key = {},
                         std::source_location here = std::source_location::current());
    /// A flexible spacer that pushes following menubar items to the right.
    WidgetRef menu_spacer(std::string_view key = {},
                          std::source_location here = std::source_location::current());
    /// Right-aligned status/meta text in a menubar (e.g. the document name).
    WidgetRef menu_meta(std::string_view text,
                        std::string_view key = {},
                        std::source_location here = std::source_location::current());

    /// A dockable panel: titled tab bar + body. `title` shows on the tab;
    /// `tabpanel_id` is the body's id (also the tab's target). Fill the body.
    Scope dock_panel(std::string_view title,
                     std::string_view tabpanel_id,
                     std::string_view classes = {},
                     std::string_view key = {},
                     std::source_location here = std::source_location::current());

    // ── Declarative docking ─────────────────────────────────────────────────
    // A dock container resolves a flat set of dockable declarations into a
    // split-tree layout and emits the dcs-dock DOM (auto-inserting splitters).
    // Dockables may be declared in ANY ORDER — each carries a DockLocation that
    // names its parent + side (or float placement) explicitly — so the result
    // is deterministic regardless of order. The declared layout is the SEED; a
    // saved Workspace layout (when wired) overrides it.

    /// Declare a dock container. Inside `build`, call document() for the
    /// center/document pane and dockpanel() for the surrounding dockables. The
    /// engine resolves the layout and emits it when `build` returns. The
    /// returned ref is adoptable as a container component for runtime
    /// add/remove of dockables.
    WidgetRef document_view(std::string_view key,
                            const std::function<void(View&)>& build,
                            std::source_location here = std::source_location::current());

    /// The center/document pane's content (only valid inside a document_view
    /// build). The dockables attach around it. `icon` is a Decius icon-font
    /// glyph for the tab (empty = none). Returns a handle whose .toolbar(...)
    /// declares the document pane's tab toolbar (e.g. a viewport's mode/tool
    /// controls).
    DockHandle document(const std::function<void(View&)>& content,
                        std::string_view title = "Document",
                        std::string_view icon = {},
                        std::source_location here = std::source_location::current());

    /// Supply the dock engine's SAVED layout — the per-pane sizes restored from
    /// the app's workspace store. The resolver consults it: a saved size for a
    /// pane id overrides the size declared in its DockLocation (returns <= 0 to
    /// fall back to the declared/default). This is how a saved arrangement wins
    /// over the declared seed.
    void set_dock_size_provider(std::function<int(std::string_view pane_id)> fn);

    /// Supply the dock engine's SAVED structure — per-panel placement overrides
    /// (docked side/parent or floating rect) produced by drag-to-dock / tearoff
    /// interactions and read back from the Document. The resolver consults it
    /// per panel: an override with `present=true` wins over the declared
    /// DockLocation, so a torn-off or re-docked arrangement survives a rebuild.
    /// This is the structural twin of set_dock_size_provider.
    void set_dock_placement_provider(
        std::function<Document::DockPlacement(std::string_view panel_id)> fn);

    /// Supply the CURRENT dock arrangement (Document::dock_layout(), read live
    /// from the DOM). When it covers every declared panel, document_view()
    /// re-emits THAT arrangement — splits, tab order, active tabs, floats —
    /// instead of the declared seed, so drag-to-dock / tearoff surgery
    /// survives view rebuilds. A stale layout (panel set changed in code)
    /// falls back to the declared seed.
    void set_dock_layout_provider(std::function<Document::DockLayout()> fn);

    /// Supply the active tab for each dock leaf. Empty selects the primary
    /// panel. When provided, inactive tab bodies are emitted as empty hidden
    /// placeholders, and their content is built only when selected.
    void set_dock_active_tab_provider(
        std::function<std::string(std::string_view pane_id)> fn);

    /// Declare a dockable panel. `where` carries everything about placement
    /// (side/parent/float) — use Dock::Left directly, or the DockLocation
    /// factories (DockLocation::docked/tab/floating/tearoff). `content` fills
    /// its body when the engine emits it. Returns a handle usable as another
    /// panel's parent and with the container's runtime API.
    DockHandle dockpanel(std::string_view title,
                         const DockLocation& where,
                         const std::function<void(View&)>& content,
                         std::string_view icon = {},
                         std::string_view key = {},
                         std::source_location here = std::source_location::current());

    /// A collapsible section: a header (chevron + title) and a body the
    /// returned Scope fills. Clicking the header toggles collapse (driven by
    /// the core interaction layer). `expanded` sets the initial state.
    Scope foldout(std::string_view title,
                  bool expanded = true,
                  std::string_view key = {},
                  std::source_location here = std::source_location::current());

    /// A vector field: a labeled row of 2–4 numeric channels (a dcs-vec of
    /// drag-scrub combos). `channels` are the per-channel tags (e.g.
    /// {"X","Y","Z"}); its size (2–4) is the channel count. `values` are the
    /// initial channel values (missing entries default to 0). Returns the vec
    /// ref; bind on_change on individual channels via component<>/find if
    /// needed.
    WidgetRef vec(std::string_view label,
                  const std::vector<std::string>& channels,
                  const std::vector<double>& values = {},
                  std::string_view key = {},
                  std::source_location here = std::source_location::current());
    /// A drag splitter between docked regions. `horizontal` splits top/bottom.
    WidgetRef splitter(bool horizontal = false,
                       std::string_view key = {},
                       std::source_location here = std::source_location::current());

    /// A tree container (selection via the interaction layer). Fill with
    /// tree_row()s.
    Scope tree(std::string_view key = {},
               std::source_location here = std::source_location::current());
    /// A selectable tree row at `depth`. The component generates the canonical
    /// row substructure (chevron / icon / label / meta). Returns the row ref;
    /// wire on_click for selection (the core `data-dcs-select` handler also
    /// tracks it) and, for an expandable row, on_click on whatever drives
    /// expand at the app level. Label-only rows can use the short overload.
    WidgetRef tree_row(std::string_view label,
                       const TreeRowOptions& opts,
                       std::string_view key = {},
                       std::source_location here = std::source_location::current());
    /// Convenience: a label-only selectable row at `depth`.
    WidgetRef tree_row(std::string_view label,
                       bool selected = false,
                       int depth = 0,
                       std::string_view key = {},
                       std::source_location here = std::source_location::current());

    /// A status bar row. Fill with text/spans.
    Scope status_bar(std::string_view key = {},
                     std::source_location here = std::source_location::current());

    /// A color field that opens a swatch picker popup. `value` is a CSS color
    /// string; `swatches` are the popup choices. Returns a ref adoptable as a
    /// ColorField. Open/close/placement handled by the interaction layer.
    WidgetRef color_field(std::string_view label,
                          std::string_view value,
                          const std::vector<std::string>& swatches,
                          std::string_view key = {},
                          std::source_location here = std::source_location::current());

    /// The Decius color field: a chip + editable hex + caret that opens an
    /// SV-square / hue-bar picker popover (the chip drag-scrubs H/S/V). `value`
    /// is a CSS hex color. For non-Decius personalities this degrades to a
    /// native color input. Returns a ColorField-adoptable ref (on_change fires
    /// with the new hex).
    WidgetRef colorfield(std::string_view label,
                         std::string_view value,
                         std::string_view key = {},
                         std::source_location here = std::source_location::current());

    /// A bare numeric combo editor (drag-scrub value + inline edit), WITHOUT a
    /// field/label wrapper — for packing several into a dcs-vec or a custom
    /// row. `label` is the short axis tag shown inside the combo (e.g. "X").
    /// Returns the combo ref (on_change fires with the new value).
    WidgetRef combo(std::string_view label,
                    double value,
                    double step = 0.01,
                    std::string_view key = {},
                    std::source_location here = std::source_location::current());

    [[nodiscard]] WidgetRef find_widget(std::string_view name);

    /// Query a live widget by id and wrap it in a strongly-typed component
    /// (see components.h). `T` must be constructible from a WidgetRef and
    /// expose `static bool matches(const WidgetNode&)` describing the widget
    /// shape it represents. The result is always safe:
    ///   - no such widget          -> a null wrapper (operator bool() false);
    ///   - widget of the WRONG type -> a diagnostic is logged and a null
    ///                                 wrapper returned (never a mismatched wrap);
    ///   - a matching widget        -> a live typed wrapper.
    /// Accessors on a null wrapper return defaults / no-op rather than crash.
    ///
    ///   if (auto blend = view.component<Dropdown>("blend"))
    ///       doc.set_blend(blend.selected());
    template <typename T>
    [[nodiscard]] T component(std::string_view name) {
        WidgetRef ref = find_widget(name);
        const WidgetNode* n = ref.node();
        if (n == nullptr) {
            return T{std::move(ref), ComponentValidity::NotPresent};
        }
        if (!T::matches(*n)) {
            // Wrong type: log, but stay ATTACHED (caller can introspect kind())
            // — just not valid; typed accessors stay inert.
            note_component_type_mismatch(name);
            return T{std::move(ref), ComponentValidity::WrongType};
        }
        return T{std::move(ref), ComponentValidity::Valid};
    }

    [[nodiscard]] std::vector<WidgetClickBinding> click_bindings() const;
    [[nodiscard]] std::vector<WidgetChangeBinding> change_bindings() const;
    [[nodiscard]] std::vector<WidgetChangeBinding> commit_bindings() const;
    [[nodiscard]] const WidgetNode* find_remote(std::string_view remote_id) const;
    [[nodiscard]] std::string to_html_fragment() const;
    [[nodiscard]] std::string to_html_document() const;
    /// to_html_document with an EMPTY <main> — the reconcile bootstrap
    /// loads this shell (head/styles/body attrs) and then replays the
    /// built tree through the document sink, so every element is known
    /// to the sink from birth and no HTML reparse ever happens again.
    [[nodiscard]] std::string to_html_shell() const;

private:
    friend class WidgetRef;

    // Records a "queried a widget by id but it was the wrong type" diagnostic
    // for the typed component<T>() query. Kept out-of-line so the header
    // template needn't see the diagnostics machinery.
    void note_component_type_mismatch(std::string_view name);

    WidgetNode& open_node(WidgetKind kind,
                          std::string_view tag,
                          std::string_view classes,
                          std::string_view key,
                          std::source_location here,
                          bool push_scope);
    void close_node();
    void close_to(std::size_t target);
    // Build a Scope owning the single open level just pushed (the common case:
    // `node` is stack_.back()). The Scope unwinds to the depth before it.
    [[nodiscard]] Scope scope_here(WidgetNode& node) {
        return Scope{this, &node, stack_.size() - 1};
    }
    void set_attr(WidgetNode& node, std::string_view name, std::string_view value);
    void set_selector(WidgetNode& node,
                      std::string_view name,
                      std::string_view value);
    void remove_attr(WidgetNode& node, std::string_view name);
    void set_text(WidgetNode& node, std::string_view value);
    void set_click_handler(WidgetNode& node, std::function<void()> cb);
    void set_change_handler(WidgetNode& node,
                            std::function<void(std::string_view)> cb);
    void set_commit_handler(WidgetNode& node,
                            std::function<void(std::string_view)> cb);
    void clear_children(WidgetNode& node);
    void set_widget_name(WidgetNode& node, std::string_view name);
    void unregister_tree(const WidgetNode& node);
    bool can_mutate_children(std::string_view operation);
    void build_children(WidgetNode& node,
                        const std::function<void(View&)>& build,
                        bool replace);
    [[nodiscard]] WidgetNode* find_id(StableId id);
    [[nodiscard]] const WidgetNode* find_id(StableId id) const;
    [[nodiscard]] WidgetRef ref_for_node(const WidgetNode& node,
                                         StableId panel_id,
                                         std::string_view name = {});
    [[nodiscard]] WidgetNode* find_widget_node_under(StableId root_id,
                                                     std::string_view name);
    [[nodiscard]] WidgetNode* resolve_widget_ref(const WidgetRef& ref);
    [[nodiscard]] ViewSink* current_sink() const noexcept;

    ViewTheme theme_{ViewTheme::Bootstrap};
    std::string framework_version_;  // empty = personality default
    // True when an app-shell widget (menubar / toolbar / statusbar /
    // document_view) was declared at ROOT level this build. Drives the
    // aui-root--shell document class: shells render edge-to-edge (no demo
    // padding, flush rows); content pages keep the padded, gapped stack.
    bool root_app_shell_{false};
    // True when any node's attrs were mutated this pass — end() then runs
    // one flush walk emitting the coalesced per-node attribute diffs.
    bool attr_coalesce_dirty_{false};
    void flush_attr_diffs(WidgetNode& node);
    std::vector<WidgetAttribute> document_attrs_;
    WidgetNode root_{};
    std::vector<WidgetNode*> stack_;
    std::vector<std::pair<std::string, StableId>> widget_names_;
    std::vector<std::pair<StableId, std::function<void()>>> click_handlers_;
    std::vector<std::pair<StableId, std::function<void(std::string_view)>>>
        change_handlers_;
    std::vector<std::pair<StableId, std::function<void(std::string_view)>>>
        commit_handlers_;
    std::vector<std::string> diagnostics_;
    ViewSink* sink_{nullptr};
    ViewSink* mutation_sink_{nullptr};
    bool reconciling_{false};
    RemotePatchSink remote_patch_sink_{};

    // Active dock-container recorder (set for the duration of a document_view
    // build callback). document()/dockpanel() record into it; the engine
    // resolves the layout into a DockNode tree and emits it when build returns.
    struct DockRecorder;
    struct DockNode;
    DockRecorder* dock_recorder_{nullptr};
    std::function<int(std::string_view)> dock_size_provider_;
    std::function<Document::DockPlacement(std::string_view)>
        dock_placement_provider_;
    std::function<std::string(std::string_view)> dock_active_tab_provider_;
    std::function<Document::DockLayout()> dock_layout_provider_;
    DockNode resolve_dock(const DockRecorder& rec, std::string_view node_id,
                          bool is_document) const;
    // Replay: convert a live Document::DockLayout into the emit tree (content
    // looked up per panel id in the recorder). Returns false into `ok` when
    // the layout doesn't cover the declared panel set (stale → seed instead).
    DockNode dock_node_from_layout(const Document::DockLayout::Node& n,
                                   const DockRecorder& rec) const;
    void emit_layout_floats(const Document::DockLayout& layout,
                            const DockRecorder& rec,
                            std::source_location here);
    // Shared emission for one floating panel (declared-seed and replay paths).
    // `anchor` set = declared seed anchored to that corner (x/y count inward
    // from it, emitted as right:/bottom: CSS); nullopt = concrete left/top px
    // (the replay path and post-drag positions).
    void emit_one_floating_panel(const DockRecorder& rec,
                                 const std::string& primary_id,
                                 const std::vector<std::string>& co_tab_ids,
                                 int x, int y, int w, int h,
                                 const std::string& active_tab,
                                 std::optional<DockCorner> anchor,
                                 std::source_location here);
    void emit_dock_node(const DockNode& node, bool is_root,
                        const DockRecorder* rec = nullptr,
                        std::source_location here =
                            std::source_location::current());
    void emit_floating_dock_panels(const DockRecorder& rec,
                                   std::source_location here);
    // Attach a tab toolbar to a recorded dockable (or the document, id
    // "__document__"). Called by DockHandle::toolbar() during the build.
    void attach_dock_toolbar(std::string_view id,
                             const std::function<void(View&)>& build);
    friend struct DockHandle;
};

}  // namespace affineui
