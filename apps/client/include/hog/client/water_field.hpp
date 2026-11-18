#pragma once

#include <hog/client/held_pages.hpp>
#include <hog/client/shape_mesh.hpp>
#include <hog/client/transvoxel.hpp>
#include <hog/core/math.hpp>
#include <hog/globe/voxel_mesh.hpp>
#include <hog/grid/grid.hpp>
#include <hog/grid/key.hpp>
#include <hog/grid/water_bodies.hpp>
#include <hog/terrain/block_mesh.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <unordered_map>

namespace hog::client {

// Open water as held pages mark it and the bodies they mark have it, read at points of the grid's
// finest lattice for blocks to mesh. A point is in the water of the body the finest leaves round it
// mark, the lowest id of those held where they mark more than one, and its value is the metres it
// stands over that water's surface at its lowest in the year; one marked with none, or none held,
// is dry. An edge between a wet point and a dry one crosses at no surface, only the end of the
// water, which water_of leaves out. Read by one thread.
class WaterField {
public:
    // The pages and bodies must outlive it.
    WaterField(const PagesSnapshot& pages, const grid::WaterBodies& bodies);

    [[nodiscard]] double value(I64Vec3 point);
    [[nodiscard]] Crossed crossing(I64Vec3 low, I64Vec3 high);
    // Reading this, which must outlive what's returned.
    [[nodiscard]] BlockField field();

private:
    // The water a point is in: metres from the planet's centre its surface stands, at its lowest,
    // how far it rises and when, and whether it's the sea.
    struct Wet {
        double level = 0.0;
        double rise = 0.0;
        double peak = 0.0;
        bool sea = false;
    };
    struct Leaf {
        grid::Water::Value body = 0;
        I64Vec3 low{0};
        I64Vec3 high{0};
    };

    [[nodiscard]] DVec3 at(I64Vec3 point) const noexcept;
    // Of the finest node at `point`, its low corner.
    [[nodiscard]] const Leaf& leaf(I64Vec3 point);
    [[nodiscard]] const std::optional<Wet>& wet(I64Vec3 point);

    const PagesSnapshot* pages_;
    const grid::WaterBodies* bodies_;
    grid::Grid grid_;
    DVec3 origin_;
    std::int64_t across_;                            // finest nodes along a side of the root
    std::unordered_map<std::uint64_t, Leaf> leaves_; // by the finest node's code
    std::unordered_map<std::uint64_t, std::optional<Wet>> wets_;
    std::array<const Leaf*, 4> recent_{};
    std::size_t next_recent_ = 0;
};

// Whether any water may be marked in the node, as the finest page over it says.
[[nodiscard]] bool may_hold_water(const PagesSnapshot& pages, grid::Key node);

// The lake's or river's water `eye` lies in, or would at its highest in the year, as the held pages
// mark it there and its body has it; none in the sea, which is drawn as its own, or in none.
[[nodiscard]] std::optional<globe::StandingWater> still_water_in(const HeldPages& held, DVec3 eye);

// A block's water surface, meshed from a WaterField, as still water draws it: each vertex rising
// as its water does through the year, the sea's marked as its own, and those where the water ends
// marked as its edge, which waves may lower but never lift.
[[nodiscard]] terrain::BlockMesh water_of(const ShapeMesh& mesh);

} // namespace hog::client
