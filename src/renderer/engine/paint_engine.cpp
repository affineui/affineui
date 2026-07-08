#include "renderer/engine/paint_engine.h"

namespace affineui {

struct PaintEngine::Impl {};

PaintEngine::PaintEngine() : impl_(std::make_unique<Impl>()) {}
PaintEngine::~PaintEngine() = default;
PaintEngine::PaintEngine(PaintEngine&&) noexcept = default;
PaintEngine& PaintEngine::operator=(PaintEngine&&) noexcept = default;

void PaintEngine::attach(BoxTree& /*boxes*/, StyleEngine& /*style*/, LayerTree& /*layers*/) {}

PaintStats PaintEngine::paint_pass() { return {}; }
void       PaintEngine::paint_subtree(BoxId /*root*/) {}

}  // namespace affineui
