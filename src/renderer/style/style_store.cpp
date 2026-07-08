// StyleStore — SoA allocation + dirty bookkeeping for all per-
// element style data. Hot-path reads are direct array indexing;
// allocation is amortized O(1) (vector push_back).

#include "renderer/style/style_store.h"

namespace affineui::detail {

namespace {
const std::string kEmptyString{};
}

ElementId StyleStore::acquire(lxb_dom_element_t* element) {
    if (auto it = by_element_.find(element); it != by_element_.end()) {
        // Already tracked. Bump dirty so the next pass refreshes.
        dirty_[it->second] = DirtyStyle | DirtyLayout | DirtyPaint
                           | DirtyRasterize | DirtyComposite;
        return ElementId{it->second, generations_[it->second]};
    }

    const std::uint32_t index = static_cast<std::uint32_t>(computed_.size());
    computed_.emplace_back();
    animated_.emplace_back();
    state_bits_.push_back(0);
    dirty_.push_back(DirtyStyle | DirtyLayout | DirtyPaint
                   | DirtyRasterize | DirtyComposite);
    // Generation starts at 1; 0 means "freed slot."
    generations_.push_back(1);
    elements_.push_back(element);
    by_element_.emplace(element, index);
    return ElementId{index, generations_.back()};
}

ElementId StyleStore::acquire_synthetic() {
    const std::uint32_t index = static_cast<std::uint32_t>(computed_.size());
    computed_.emplace_back();
    animated_.emplace_back();
    state_bits_.push_back(0);
    dirty_.push_back(DirtyStyle | DirtyLayout | DirtyPaint
                   | DirtyRasterize | DirtyComposite);
    generations_.push_back(1);
    elements_.push_back(nullptr);
    // No by_element_ entry — synthetic slots aren't reverse-lookupable.
    return ElementId{index, 1};
}

void StyleStore::reset() {
    computed_.clear();
    animated_.clear();
    state_bits_.clear();
    dirty_.clear();
    generations_.clear();
    elements_.clear();
    by_element_.clear();
    font_families_.clear();
    font_families_.emplace_back("sans");
}

ElementId StyleStore::lookup(lxb_dom_element_t* element) const {
    auto it = by_element_.find(element);
    if (it == by_element_.end()) return {};
    return ElementId{it->second, generations_[it->second]};
}

lxb_dom_element_t* StyleStore::element_of(ElementId id) const {
    if (!id.valid() || id.index >= generations_.size()) return nullptr;
    if (generations_[id.index] != id.generation) return nullptr;
    // O(1) — this sits on the hover/hit-test path (called per block per
    // pointer event). The old linear reverse scan of by_element_ was 60%
    // of total CPU in a profiled interactive session.
    return elements_[id.index];
}

ComputedStyle&       StyleStore::computed(ElementId id)         { return computed_[id.index]; }
const ComputedStyle& StyleStore::computed(ElementId id) const   { return computed_[id.index]; }
AnimatedStyle&       StyleStore::animated(ElementId id)         { return animated_[id.index]; }
const AnimatedStyle& StyleStore::animated(ElementId id) const   { return animated_[id.index]; }
std::uint8_t&        StyleStore::state_bits(ElementId id)       { return state_bits_[id.index]; }
std::uint8_t         StyleStore::state_bits(ElementId id) const { return state_bits_[id.index]; }
std::uint8_t&        StyleStore::dirty(ElementId id)            { return dirty_[id.index]; }
std::uint8_t         StyleStore::dirty(ElementId id) const      { return dirty_[id.index]; }

void StyleStore::mark_dirty(ElementId id, std::uint8_t flags) {
    if (!id.valid() || id.index >= dirty_.size()) return;
    if (generations_[id.index] != id.generation) return;
    dirty_[id.index] |= flags;
}

std::uint8_t StyleStore::intern_font_family(const std::string& family) {
    for (std::size_t i = 0; i < font_families_.size(); ++i) {
        if (font_families_[i] == family) return static_cast<std::uint8_t>(i);
    }
    if (font_families_.size() >= 255) return 0;  // overflow → default
    font_families_.push_back(family);
    return static_cast<std::uint8_t>(font_families_.size() - 1);
}

const std::string& StyleStore::font_family_of(std::uint8_t id) const {
    if (id >= font_families_.size()) return kEmptyString;
    return font_families_[id];
}

}  // namespace affineui::detail
