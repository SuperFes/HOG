#pragma once

#include <hog/core/math.hpp>
#include <hog/planet/cell_id.hpp>
#include <hog/terrain/bend.hpp>
#include <hog/terrain/patch.hpp>
#include <hog/terrain/stitching.hpp>

#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

namespace hog::terrain {

// Where the patches are seen from, with the planet centred on the origin.
struct Viewpoint {
    WorldPosition eye;
    DVec3 forward{1.0, 0.0, 0.0};     // unit
    DVec3 up{0.0, 0.0, 1.0};          // unit, not along forward
    double vertical_fov = 1.0;        // radians
    double aspect = 16.0 / 9.0;       // width over height
    double viewport_height = 1'080.0; // pixels
    // How the ground is bent as it's drawn, which is what's culled and refined.
    // NOLINTNEXTLINE(readability-redundant-member-init): lets designated initializers omit it
    Bend bend = {};
};

struct SelectionSettings {
    // Pixels a patch's vertex spacing may span on screen before the patch splits.
    double max_error = 8.0;
    // Metres between vertices below which patches stop splitting.
    double finest_spacing = 0.25;
    // Metres from sea level the ground is assumed to stay between, for the patches' bounds;
    // generous over the generated ground, so only changes beyond it can be culled too early.
    double lowest = -6'000.0;
    double highest = 9'000.0;
};

// What some cells' patches are known to keep between, such as from baking them, tighter than the
// settings' range. A patch's ground lies within its children's.
using KnownHeights = std::function<std::optional<HeightRange>(planet::CellId)>;

struct SelectedPatch {
    planet::CellId cell;
    std::uint8_t coarser_edges = 0; // edge bits, where the neighbour is a level coarser
    double distance = 0.0;          // least metres from the eye to the patch's ground
};

// The patches to draw: a quadtree over the six faces, split until each patch's vertex spacing is
// within the error on screen, then balanced so neighbours differ by at most a level. Patches
// outside the view or below the horizon are left out. Nearest first.
[[nodiscard]] std::vector<SelectedPatch> select_patches(const Viewpoint& view, double planet_radius,
                                                        const SelectionSettings& settings = {},
                                                        const KnownHeights& known = {});

} // namespace hog::terrain
