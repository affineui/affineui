#include "renderer/engine/rasterizer.h"

namespace affineui {

struct Rasterizer::Impl {};

Rasterizer::Rasterizer(RasterizerConfig cfg)
    : impl_(std::make_unique<Impl>()), cfg_(cfg) {}
Rasterizer::~Rasterizer() = default;
Rasterizer::Rasterizer(Rasterizer&&) noexcept = default;
Rasterizer& Rasterizer::operator=(Rasterizer&&) noexcept = default;

void Rasterizer::init(FontRegistry& /*fonts*/, ImageRegistry& /*images*/) {}
void Rasterizer::shutdown() {}

RasterizeStats Rasterizer::rasterize_pass(LayerTree& layers, float /*dpi_scale*/) {
    RasterizeStats s{};
    // Phase-2 stub: walk dirty layers, mark them clean. Actual NanoVG
    // rasterization lands once the dep-fetched build path is wired.
    for (std::size_t i = 0; i < layers.size(); ++i) {
        auto& layer = layers.at(static_cast<LayerId>(i));
        if (!layer.dirty_raster()) {
            ++s.layers_skipped_hash_match;
            continue;
        }
        if (layer.display_list.rolling_hash() == layer.rasterized_hash
            && layer.rasterized_hash != 0) {
            ++s.layers_skipped_hash_match;
            layer.clear_raster_dirty();
            continue;
        }
        layer.rasterized_hash = layer.display_list.rolling_hash();
        layer.clear_raster_dirty();
        ++s.layers_rasterized;
    }
    return s;
}

void Rasterizer::rasterize_layer(Layer& /*layer*/, float /*dpi_scale*/) {}

void Rasterizer::return_to_pool(LayerTextureHandle /*tex*/, SizeF /*px_size*/) noexcept {}

std::uint64_t Rasterizer::vram_in_use_bytes() const noexcept { return 0; }

}  // namespace affineui
