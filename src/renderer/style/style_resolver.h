#pragma once

#include "renderer/style/animated_style.h"
#include "renderer/style/computed_style.h"

#include <memory>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

// Forward declarations so callers don't drag in lexbor headers.
struct lxb_html_document;
typedef struct lxb_html_document lxb_html_document_t;
struct lxb_dom_element;
typedef struct lxb_dom_element lxb_dom_element_t;
struct lxb_css_rule_declaration_list;
typedef struct lxb_css_rule_declaration_list lxb_css_rule_declaration_list_t;

namespace affineui::detail {

/// CSS custom properties (`--name: value`) in effect for an element,
/// keyed by name (including the leading `--`) with the raw, unresolved
/// value string. They inherit, so the cascade carries them down the
/// tree; `var()` references are substituted against this map at
/// resolve time. Stored behind a shared_ptr so inheritance is a cheap
/// pointer copy — only an element that *declares* custom properties
/// clones the map (copy-on-write).
using CustomPropMap = std::unordered_map<std::string, std::string>;

struct BoxShadowLayer {
    std::uint32_t rgba{0x00000000u};
    std::int16_t offset_x{0};
    std::int16_t offset_y{0};
    std::int16_t blur{0};
    std::int16_t spread{0};
    bool inset{false};
};

using BoxShadowList = std::vector<BoxShadowLayer>;

/// One color stop of a CSS gradient. `offset` is the normalised position
/// along the ramp (0–1). Positions are resolved from CSS percentages at
/// cascade time; stops without an explicit position are distributed
/// evenly between their neighbours (CSS gradient stop-placement rules).
struct GradientStop {
    float         offset{0.0f};
    std::uint32_t rgba{0x00000000u};
};

/// Full ordered stop list for a gradient with more than two stops. Two-
/// stop gradients stay entirely inside AnimatedStyle's inline
/// stop0/stop1 fields (zero side-table lookup for the common case); only
/// gradients with 3+ stops allocate this list at cascade time and carry
/// it out-of-line, exactly like `BoxShadowList`.
using GradientStopList = std::vector<GradientStop>;

/// One gradient background layer painted OVER the bottom layer.
///
/// CSS `background` / `background-image` is a comma-separated, back-to-
/// front stack of image layers: the LAST value in the list is the bottom-
/// most and the FIRST is on top. We split that stack in two:
///
///   - The BOTTOM layer keeps the legacy inline home it has always had —
///     AnimatedStyle's gradient_kind/angle/center/stop0/stop1 fields,
///     plus `gradient_stops` when it has 3+ stops. That is the case for
///     virtually every element in a real UI, and it costs zero side-table
///     lookups and (at 2 stops) zero allocations. Do not regress it.
///   - Every layer ABOVE the bottom lands in `background_layers`, one
///     entry each, in CSS source order (index 0 = topmost). Null for the
///     single-layer case, exactly like `box_shadows`.
///
/// This replaced an earlier `OverlayGradient` that hardcoded exactly two
/// stops and only ever held ONE layer. It was written for the color-
/// picker square (a genuinely 2-stop, 2-layer value) and silently
/// truncated anything richer: the Decius skeuomorphic panels
/// (`.dcs-hw--lacquer` / `--brushed`) stack a 6-stop specular highlight
/// over a 3-stop base, and truncating that highlight to its first and
/// last stop turns a subtle sheen into a blown-out white wash — the CSS
/// ramp falls from 42% white to 1.5% by 70% of the radius, while a 2-stop
/// approximation carries 42% white linearly across the whole panel.
/// Layers therefore carry their own full stop list.
struct BackgroundLayer {
    enum class Kind : std::uint8_t {
        None = 0,
        Linear,          ///< linear-gradient()
        Radial,          ///< radial-gradient()
        LinearStripes,   ///< repeating-linear-gradient() tile approximation
    };

    Kind kind{Kind::None};
    /// Linear: CSS gradient angle, degrees, 0 = upward, clockwise.
    std::int16_t angle_deg{0};
    /// Radial: gradient centre as a percentage of the box. SIGNED and
    /// unclamped on purpose — CSS routinely places a specular highlight
    /// just OUTSIDE the box (`at 50% -10%`) so only the bottom edge of
    /// the falloff lands on it. Clamping that to 0 pulls the hot centre
    /// onto the panel's top edge and is exactly the sort of blow-out this
    /// struct exists to avoid.
    std::int16_t center_x_pct{50};
    std::int16_t center_y_pct{50};
    /// Radial: the ending shape's radii, as a percentage of the box's
    /// half-width / half-height (`ellipse 110% 90%` → 110 / 90). 0 means
    /// "no explicit size" — use the CSS default, farthest-corner.
    std::uint16_t radius_x_pct{0};
    std::uint16_t radius_y_pct{0};
    /// The layer's full ordered ramp. Always populated (2+ stops for any
    /// kind other than None); offsets have already had CSS stop-placement
    /// applied, so they are ascending in 0–1.
    GradientStopList stops;
};

/// The above-the-bottom part of an element's background stack, topmost
/// first (CSS source order). Carried out-of-line behind a shared_ptr and
/// left null for the overwhelmingly common single-layer case — the same
/// "most elements have none" idiom as `BoxShadowList`.
using BackgroundLayerList = std::vector<BackgroundLayer>;

/// The two-struct bundle the cascade resolves into. Splitting them
/// pays off downstream: layout reads ComputedStyle only, paint reads
/// AnimatedStyle only, composite reads only the transform/opacity
/// fields out of AnimatedStyle. See docs/DESIGN.md §
/// "Real-time render architecture."
struct ResolvedStyle {
    ComputedStyle computed{};
    AnimatedStyle animated{};
    struct CssAnimation {
        enum class Timing : std::uint8_t {
            Ease = 0, Linear, EaseIn, EaseOut, EaseInOut, StepStart, StepEnd,
        };
        enum class Direction : std::uint8_t {
            Normal = 0, Reverse, Alternate, AlternateReverse,
        };
        enum class FillMode : std::uint8_t {
            None = 0, Forwards, Backwards, Both,
        };
        enum class PlayState : std::uint8_t {
            Running = 0, Paused,
        };

        std::uint32_t name_hash{0};
        float         duration_s{0.0f};
        float         delay_s{0.0f};
        float         iteration_count{1.0f};  // 0 = infinite
        Timing        timing{Timing::Ease};
        Direction     direction{Direction::Normal};
        FillMode      fill_mode{FillMode::None};
        PlayState     play_state{PlayState::Running};
        bool          active{false};
    } animation{};
    /// Inherited custom-property scope (null = none in effect).
    std::shared_ptr<const CustomPropMap> custom_props;
    /// Optional multi-layer box-shadow list. Most elements have no
    /// shadow, and single-layer shadows stay in AnimatedStyle's compact
    /// legacy fields; only stacked shadows allocate this list at resolve time.
    std::shared_ptr<const BoxShadowList> box_shadows;
    /// Optional N-stop (>2) gradient stop list for the element's bottom
    /// background layer. Null for the common 2-stop / no-gradient case,
    /// which is served entirely by AnimatedStyle's inline stop0/stop1.
    /// Ordered by ascending offset; the offsets have already had CSS
    /// stop-placement (even distribution / monotonic clamping) applied.
    std::shared_ptr<const GradientStopList> gradient_stops;
    /// Optional background layers stacked ABOVE the bottom one, topmost
    /// first. Null for the common single-layer case — see BackgroundLayer.
    std::shared_ptr<const BackgroundLayerList> background_layers;
};

struct ViewportDependency {
    bool width{false};
    bool height{false};
};

/// Abstract style resolver. Phase 2 ships one impl (lexbor-backed)
/// that delegates to lexbor's cascade via `lxb_html_element_style_walk`.
/// Future phases can swap in a custom matcher with bloom filters,
/// invalidation sets, computed-style sharing — the rest of the
/// engine sees only this interface.
class StyleResolver {
public:
    virtual ~StyleResolver() = default;

    /// Resolve the full style for `element`, merging in inherited
    /// properties from `parent`. Implementations may cache the static
    /// cascade result; callers must use `invalidate()` when DOM attributes
    /// or styles that can affect matching are mutated.
    virtual ResolvedStyle resolve(lxb_dom_element_t* element,
                                  const ResolvedStyle& parent) = 0;

    /// Apply every declaration in `list` to `out` in source order.
    /// Used by the :hover overlay pass to layer state-dependent rules
    /// on top of an already-resolved base style without re-running the
    /// full cascade. Caller is responsible for whether the rule's
    /// selector should be applied at all.
    virtual void apply_decl_list(
        const lxb_css_rule_declaration_list_t* list,
        ResolvedStyle& out) = 0;

    /// Mark an element's style cache entry dirty so the next
    /// `resolve()` call recomputes. Phase 2 doesn't yet propagate
    /// to descendants.
    virtual void invalidate(lxb_dom_element_t* element) = 0;

    /// Drop all caches. Called by Document::set_html when the DOM
    /// is wholesale replaced.
    virtual void clear() = 0;

    /// Reports whether any declaration actually resolved by this resolver
    /// used viewport-relative units. This lets Document::layout avoid
    /// rebuilding the entire block/style tree on resize ticks where the
    /// active media query set is unchanged and the resized dimension cannot
    /// affect computed style.
    virtual ViewportDependency viewport_dependency() const { return {}; }

    /// Update the CSS viewport used by future resolutions without clearing
    /// existing cached styles. Callers pair this with viewport_dependency():
    /// cached styles can be retained when no cached value depends on the
    /// changed dimension, while future dynamic resolves still see the current
    /// viewport.
    virtual void set_viewport(int /*width_px*/, int /*height_px*/) {}
};

/// Construct the default resolver, backed by lexbor's cascade.
std::unique_ptr<StyleResolver> make_lexbor_resolver(
    lxb_html_document_t* doc,
    int viewport_width_px = 0,
    int viewport_height_px = 0);

}  // namespace affineui::detail
