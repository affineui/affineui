#pragma once

#include <cstdint>
#include <memory>
#include <span>
#include <utility>

namespace affineui {

namespace detail {
class ImageLease;
class ImageRegistry;
}

/// An invalidating handle to a renderer-owned RGBA image.
///
/// The handle never owns or exposes a Painter. It becomes invalid when the
/// image is reset, its renderer shuts down, or the renderer is destroyed.
/// Operations on an invalid handle are safe no-ops and return false.
/// Copies share ownership; the final release is marshalled to the renderer
/// thread before touching the GPU backend. Like the rest of AffineUI's UI API,
/// updates and drawing belong on the renderer/UI thread.
class ImageHandle {
public:
    ImageHandle() noexcept = default;

    [[nodiscard]] bool is_valid() const noexcept;
    [[nodiscard]] explicit operator bool() const noexcept {
        return is_valid();
    }

    /// Replace the complete tightly-packed RGBA8 payload. The byte count must
    /// exactly match width * height * 4 from creation.
    bool update(std::span<const std::uint8_t> rgba) const;

    /// Release this handle's ownership. The GPU image is destroyed when the
    /// last handle copy releases it. Calls after renderer shutdown are safe.
    void reset() noexcept;

    /// The painter-level image id — exactly what
    /// `Painter::draw_image(std::uint32_t, ...)` takes. 0 when invalid.
    ///
    /// Exposed so a native rendering core can draw an app-owned image while
    /// linking NOTHING of the affineui runtime: it needs only the abstract
    /// Painter (pure-virtual, so vtable dispatch) plus this id. The photoedit
    /// raster core does exactly that — see examples/core/photoedit.
    [[nodiscard]] std::uint32_t id() const noexcept { return backend_id(); }

private:
    explicit ImageHandle(std::shared_ptr<detail::ImageLease> lease) noexcept
        : lease_(std::move(lease)) {}

    [[nodiscard]] std::uint32_t backend_id() const noexcept;

    std::shared_ptr<detail::ImageLease> lease_{};

    friend class Painter;
    friend class detail::ImageLease;
    friend class detail::ImageRegistry;
};

}  // namespace affineui
