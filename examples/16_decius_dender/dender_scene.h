#pragma once

// DENDER scene helpers — the DENDER-specific presentation + registry logic
// that used to live on DenderDocument, now expressed as free functions over
// the shared app::Document. The Blender-style catalog (primitive types +
// spawn positions), the icon glyph mapping, the outliner depth walk, name
// uniquification (".001" suffixes), and monotonic obj_N id minting. TRS lives
// on the e3d viewport node (like the game editor), so it is NOT stored here.

#include "app/document.h"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace dender {

struct Vec3 {
    double x{0.0};
    double y{0.0};
    double z{0.0};
};

/// DENDER-local timeline state (a pure DENDER concept; kept off the shared
/// hub). Shared between the controller and the presentational components.
struct Timeline {
    int start{1};
    int end{250};
    int frame{24};
    std::vector<int> keys{1, 24, 48, 72, 96, 130, 175, 220};
};

/// One entry of the Add-menu catalog (the web app.js runtime menu).
struct Primitive {
    std::string_view type;
    std::string_view section;  // "Mesh" / "Light" / "" (top level)
    Vec3 spawn;
};

/// The runtime Add menu from app.js with viewport.js's default spawn
/// positions.
[[nodiscard]] const std::vector<Primitive>& catalog();
[[nodiscard]] const Primitive* find_primitive(std::string_view type);
/// Decius icon glyph for an object type (web ICON_FOR).
[[nodiscard]] std::string_view icon_for(std::string_view type);
[[nodiscard]] bool is_mesh(std::string_view type);

/// Outliner order: depth-first from the collection roots, registry order
/// within a level. `depth` is 0 for roots (the outliner adds its own base
/// indent for Scene > Collection).
struct OrderedObject {
    const app::Object* object{nullptr};
    int depth{0};
};
[[nodiscard]] std::vector<OrderedObject> ordered_objects(
    const app::Document& doc);

/// Unique name against the other objects (Blender / web uniqueName): keeps
/// `base` if free, else appends ".001", ".002", … to its numeric-suffix stem.
[[nodiscard]] std::string unique_name(const app::Document& doc,
                                      std::string_view base,
                                      std::string_view ignore_id);

// ── Small parse helpers (shared by the view/handlers) ────────────────────────
[[nodiscard]] bool parse_bool(std::string_view value) noexcept;
[[nodiscard]] double parse_double_or(std::string_view value,
                                     double fallback) noexcept;

}  // namespace dender
