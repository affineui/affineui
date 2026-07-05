#pragma once

#include "internal/animated_style.h"
#include "internal/computed_style.h"
#include "internal/element_id.h"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

// Forward-declared so the public header doesn't pull in lexbor.
struct lxb_dom_element;
typedef struct lxb_dom_element lxb_dom_element_t;

namespace affineui::detail {

/// SoA storage for all per-element style + state data. One instance
/// lives on each Document; arrays are indexed by `ElementId::index`.
///
/// Dirty bits gate the five-stage pipeline (Style / Layout / Paint /
/// Rasterize / Composite). Each per-element entry tracks its own
/// dirty mask so a paint-only property change doesn't invalidate
/// layout.
///
/// Sized to scale to 10k+ elements without behavior change — see
/// docs/DESIGN.md § "Memory footprint at scale".
class StyleStore {
public:
    enum Dirty : std::uint8_t {
        DirtyStyle     = 1u << 0,
        DirtyLayout    = 1u << 1,
        DirtyPaint     = 1u << 2,
        DirtyRasterize = 1u << 3,
        DirtyComposite = 1u << 4,
    };

    /// Allocate or recycle a slot for the given lexbor element.
    /// First call for an element creates a new ID; subsequent calls
    /// return the same ID. Marks the slot fully dirty.
    ElementId acquire(lxb_dom_element_t* element);

    /// Allocate a fresh slot not bound to any DOM element. Used for
    /// synthetic blocks (line-box wrappers for runs of inline /
    /// inline-block children). Each call returns a unique ID; the
    /// slot doesn't participate in lookup() / element_of().
    ElementId acquire_synthetic();

    /// Release every slot; arrays keep their capacity for reuse.
    /// Called by Document::set_html when the DOM is wholesale
    /// replaced.
    void reset();

    /// Look up an existing ID by element pointer. Returns an
    /// invalid ElementId if the element isn't tracked.
    ElementId lookup(lxb_dom_element_t* element) const;

    /// Resolve an ID back to the lexbor element. Returns nullptr if
    /// the ID is stale.
    lxb_dom_element_t* element_of(ElementId id) const;

    // ── Per-element accessors — indexed by ElementId::index ──────

    std::size_t       size()           const noexcept { return computed_.size(); }
    ComputedStyle&    computed(ElementId id);
    AnimatedStyle&    animated(ElementId id);
    std::uint8_t&     state_bits(ElementId id);
    std::uint8_t&     dirty(ElementId id);

    const ComputedStyle& computed(ElementId id) const;
    const AnimatedStyle& animated(ElementId id) const;
    std::uint8_t         state_bits(ElementId id) const;
    std::uint8_t         dirty(ElementId id) const;

    /// Mark an element dirty along one or more pipeline stages.
    void mark_dirty(ElementId id, std::uint8_t flags);

    /// Bulk iteration — pass a callable `(ElementId, ComputedStyle&,
    /// AnimatedStyle&)`. Sequential access for cache friendliness.
    template <typename F>
    void each(F&& f) {
        const std::size_t n = computed_.size();
        for (std::size_t i = 0; i < n; ++i) {
            if (generations_[i] == 0) continue;  // freed slot
            const ElementId id{static_cast<std::uint32_t>(i), generations_[i]};
            f(id, computed_[i], animated_[i]);
        }
    }

    // ── Font registry (font_id → resolved face name) ─────────────
    /// Look up (or assign) a u8 font_id for the given family name.
    /// Capped at 255 distinct families per document; overflow falls
    /// back to font_id 0.
    std::uint8_t intern_font_family(const std::string& family);
    const std::string& font_family_of(std::uint8_t id) const;

private:
    // Parallel SoA arrays. Index is ElementId::index.
    std::vector<ComputedStyle>    computed_;
    std::vector<AnimatedStyle>    animated_;
    std::vector<std::uint8_t>     state_bits_;   // :hover/:active/:focus
    std::vector<std::uint8_t>     dirty_;        // Dirty mask
    std::vector<std::uint16_t>    generations_;  // 0 = freed slot
    std::vector<lxb_dom_element_t*> elements_;   // slot → element (O(1)
                                                 // element_of; null for
                                                 // synthetic slots)

    // Reverse map for lookup() — lexbor element → ElementId.
    std::unordered_map<lxb_dom_element_t*, std::uint32_t> by_element_;

    // Font registry. Index = font_id (1..255), value = family string.
    // Index 0 is reserved for "sans" / default.
    std::vector<std::string>      font_families_{"sans"};
};

}  // namespace affineui::detail
