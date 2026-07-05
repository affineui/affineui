// affineui_skeuo.h — skeuomorphic hardware widget kit.
//
// Builders + typed components for hardware-emulation UIs: modular
// synths, drum machines, guitar pedals, mixing consoles. Reproduces
// the Decius CSS skeuomorphic vocabulary (deciuscss.com "Skeuomorphic
// hardware" showpiece) as native AffineUI components:
//
//   hw_panel()    — chassis faceplate (steel/brushed/lacquer/cream/red
//                   enamel finishes) with corner screws
//   silk()        — 1970s silkscreen group box with legend
//   hw_label()    — etched/engraved panel label
//   jack()        — TS/TRS patch socket with chrome hex nut (SVG art)
//   lit_button()  — backlit plastic button (red/cyan/amber/green glow)
//   lcd()         — 7-segment LED display (red/amber/green/blue)
//   led_meter()   — LED bargraph meter with ok/warn/peak zones
//   fader()       — vertical synth fader (core drag interaction)
//   step_pair()   — chunky up/down stepper buttons
//   (knobs: use the core View::knob — already skeuomorphic under
//    data-dcs-style="3d")
//
// PatchBay — Reason-style patch cabling between jacks: drag from one
// jack to another to connect, grab a patched jack to re-route, click a
// cable to remove it. Cables hang on a constant-length catenary and
// swing on a spring-damper while dragged (ported from the decius.css
// site demo). Cable rendering is a custom-paint (canvas) surface: the
// geometry is drawn app-side through the Painter vector API every
// repaint — floats end to end, no DOM churn — while static jack art
// stays retained SVG. The drag runs on App::on_event + App::on_frame.
//
// This kit is Decius-only by design (personality tier 3): it emits the
// dcs-* skeuomorphic classes directly. Views must use ViewTheme::Decius
// and the panel root should set data-dcs-style="3d" for the full look.

#pragma once

#include <affineui/app.h>
#include <affineui/components.h>
#include <affineui/view.h>

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace affineui::skeuo {

/// Kit stylesheet — rules for compositions this kit defines beyond the
/// framework CSS (keyboard strip, small lit-button typography). Append
/// it to your app CSS: `app.set_stylesheet(my_css + sk::stylesheet())`.
[[nodiscard]] std::string_view stylesheet();

// ── Enums ────────────────────────────────────────────────────────────
enum class HwFinish { Steel, Brushed, Lacquer, Cream, Red };
enum class LitColor { Red, Cyan, Amber, Green };
enum class LcdColor { Red, Amber, Green, Blue };
enum class LcdSize  { Sm, Md, Lg };

// ── Chassis / structure builders ─────────────────────────────────────
/// Open a hardware chassis panel (`.dcs-hw`) in the given finish, with
/// the four corner screws already stamped. Fill it, then let the Scope
/// close. `classes` appends extra utility classes.
View::Scope hw_panel(View& v, HwFinish finish,
                     std::string_view classes = {},
                     std::string_view key = {});

/// Open a silkscreen group box (`fieldset.dcs-silk` + legend).
View::Scope silk(View& v, std::string_view title,
                 std::string_view key = {});

/// Etched/engraved hardware label text.
WidgetRef hw_label(View& v, std::string_view text,
                   std::string_view key = {});

/// Small uppercase control label (the text under jacks and knobs).
WidgetRef control_label(View& v, std::string_view text,
                        std::string_view key = {});

// ── Controls ─────────────────────────────────────────────────────────
/// Backlit plastic button. `name` is the widget name for on_click
/// binding (WidgetRef::on_click). `lit` drives the glow.
WidgetRef lit_button(View& v, std::string_view name,
                     std::string_view glyph,
                     LitColor color = LitColor::Red,
                     bool lit = false,
                     bool small = false,
                     std::string_view key = {});

/// 7-segment LED display. `digits` > 0 left-pads the value with blank
/// digits to a fixed width. Supports 0-9, A-Z subset, '-', '.', ' '.
WidgetRef lcd(View& v, std::string_view value,
              LcdSize size = LcdSize::Md,
              LcdColor color = LcdColor::Red,
              int digits = 0,
              std::string_view key = {});

/// LED bargraph meter; `value01` in [0,1] lights the segments.
WidgetRef led_meter(View& v, float value01,
                    int segments = 14,
                    bool vertical = false,
                    std::string_view key = {});

/// Vertical synth fader (`.dcs-fader`). The core interaction layer
/// (data-dcs-fader) handles the drag and emits widget change events
/// under `name`.
WidgetRef fader(View& v, std::string_view name,
                double value, double min = 0.0, double max = 1.0,
                int height_px = 96,
                std::string_view key = {});

/// Chunky plastic up/down stepper (`.dcs-step`). `up_name`/`down_name`
/// are widget names to bind on_click handlers against the returned
/// refs (first = up, second = down).
std::pair<WidgetRef, WidgetRef> step_pair(View& v,
                                          std::string_view up_name,
                                          std::string_view down_name,
                                          std::string_view key = {});

/// Piano keyboard strip (`.skeuo-kbd`, styled by sk::stylesheet()).
/// `octaves` full octaves starting at MIDI octave `first_octave` (C3 =
/// MIDI 48 at first_octave 3). `on_note` fires with the MIDI note when
/// a key is pressed; the pressed look itself is pure :active CSS.
WidgetRef keyboard(View& v, std::string_view name,
                   int octaves,
                   std::function<void(int midi)> on_note,
                   int first_octave = 3,
                   std::string_view key = {});

// ── Typed component wrappers (components.h style) ────────────────────
class LitButton : public Component {
public:
    using Component::Component;
    static bool matches(const WidgetNode& n);
    [[nodiscard]] bool lit() const;
    void set_lit(bool lit);
};

class Lcd : public Component {
public:
    using Component::Component;
    static bool matches(const WidgetNode& n);
    [[nodiscard]] std::string value() const;
};

class LedMeter : public Component {
public:
    using Component::Component;
    static bool matches(const WidgetNode& n);
    [[nodiscard]] float value() const;
};

// ── Patch bay ────────────────────────────────────────────────────────
/// One patched cable between two jacks. `color` indexes the kit's
/// cable palette.
struct Cable {
    std::string from;
    std::string to;
    int         color{0};
};

/// Reason-style patch cabling over a board of jacks.
///
/// Declare-side (inside your view build):
///   auto board = bay.board(view);          // relative container
///   ... panels with bay.jack(view, "cv-out", "CV OUT") ...
///   bay.cables_layer(view);                 // LAST child of the board
///
/// Runtime:
///   bay.attach(app);                        // once, after App exists
///   bay.request_render = [&]{ reload(); };  // your view rebuild fn
///   bay.on_change = [&]{ ... };             // patching changed
///
/// The drag lifecycle (grab jack → cable follows with hanging physics →
/// drop on another jack) and click-to-remove are handled internally.
class PatchBay {
public:
    PatchBay();

    // ── Declaration ──
    /// Open the patch board container. Jacks declared inside it (via
    /// this bay) become patch points. Re-entrant per rebuild.
    View::Scope board(View& v, std::string_view classes = {},
                      std::string_view key = {});

    /// A patch jack registered with this bay. `jack_id` must be unique
    /// within the bay; `label` renders under the socket.
    WidgetRef jack(View& v, std::string_view jack_id,
                   std::string_view label = {},
                   std::string_view key = {});

    /// The cable overlay — a custom-paint (canvas) surface. Declare as
    /// the LAST child of the board so cables draw above the panels.
    /// attach() registers the paint handler; cable/drag geometry then
    /// repaints via App::request_custom_repaint without any rebuild.
    void cables_layer(View& v, std::string_view key = {});

    // ── Model ──
    /// Connect two jacks (replaces any existing cable on either jack).
    /// color < 0 picks the next palette colour.
    void connect(std::string_view from, std::string_view to,
                 int color = -1);
    void disconnect(std::string_view jack_id);
    void clear();
    [[nodiscard]] const std::vector<Cable>& cables() const {
        return cables_;
    }
    [[nodiscard]] bool patched(std::string_view jack_id) const;

    // ── Runtime ──
    /// Install the drag interaction + physics tick on the app. Call
    /// once. `request_render` must be set (or set it before the first
    /// interaction) to your view-rebuild function.
    void attach(App& app);

    /// Called whenever the bay needs the owning view rebuilt (cable
    /// added/removed, drag moved). Typically your reload() lambda.
    std::function<void()> request_render;

    /// Called after the user changed the patching (connect/disconnect
    /// via interaction, not via the model API).
    std::function<void()> on_change;

    /// Cable palette (RGB hex like "#6fb3ff"). Defaults to the six
    /// deciuscss.com cable colours.
    std::vector<std::string> palette;

private:
    struct Drag {
        std::string anchor;        // fixed-end jack id
        int         color{0};
        float       target_x{0.0f}, target_y{0.0f};  // plug (doc coords)
        float       bob_x{0.0f}, bob_y{0.0f};        // swing midpoint
        float       vel_x{0.0f}, vel_y{0.0f};
        bool        active{false};
    };

    bool handle_event(App& app, const Event& ev,
                      const std::vector<Document::HoverInfo>& chain);
    void tick(double dt);
    void paint_cables(Painter& painter, const Rect& bounds);
    bool jack_center(std::string_view jack_id, float& x, float& y) const;
    std::optional<std::string> jack_at(float doc_x, float doc_y) const;
    int cable_at(float doc_x, float doc_y) const;
    int next_color() const;

    App*                     app_{nullptr};
    std::vector<Cable>       cables_;
    std::vector<std::string> jacks_;       // registered ids, rebuild order
    std::string              board_name_;  // data-aui-name of the board
    std::string              canvas_name_; // data-aui-paint of the overlay
    Drag                     drag_;
    std::vector<float>       path_scratch_;  // reused cmd buffer (no
                                             // per-frame allocation)
    // Jack centers cache — find_element_rect walks the whole DOM, far
    // too slow to repeat per jack per painted frame. Filled lazily,
    // dropped whenever the view rebuilds (cables_layer) or the board
    // rect moves (paint_cables checks once per paint).
    struct JackPos { float x{0.0f}, y{0.0f}; };
    mutable std::unordered_map<std::string, JackPos> jack_pos_cache_;
    mutable Rect             cached_board_rect_{};
};

}  // namespace affineui::skeuo
