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

/// A second gradient background layer painted OVER the bottom gradient.
/// CSS `background` is a back-to-front stack of image layers; the bottom
/// layer lives in AnimatedStyle's inline gradient fields, and this
/// carries a single 2-stop overlay layer for the few widgets that need
/// one (the color-picker square: a `to top, #000, transparent` value
/// shade over the `to right, #fff, hue` saturation ramp). Kept out-of-
/// line (like box_shadows) because almost no element has an overlay, so
/// AnimatedStyle stays compact. kind: 1 = linear, 2 = radial.
struct OverlayGradient {
    std::uint8_t  kind{0};
    std::uint8_t  center_x_pct{50};
    std::uint8_t  center_y_pct{50};
    std::uint8_t  stop1_pos_pct{100};
    std::int16_t  angle_deg{0};
    std::uint16_t pad{0};
    std::uint32_t stop0_rgba{0};
    std::uint32_t stop1_rgba{0};
};

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
    /// Optional second (overlay) gradient background layer. Null for the
    /// common single-layer case.
    std::shared_ptr<const OverlayGradient> overlay_gradient;
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
