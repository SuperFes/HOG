#pragma once

#include <hog/client/held_pages.hpp>
#include <hog/client/transvoxel.hpp>
#include <hog/core/math.hpp>
#include <hog/grid/grid.hpp>
#include <hog/grid/key.hpp>
#include <hog/grid/kinds.hpp>

#include <cstdint>
#include <unordered_map>

namespace hog::client {

// The ground as held pages have it, read at points of the grid's finest lattice for blocks to
// mesh. A point's value is the mean of the distances, in metres, from the planes of the finest
// leaves round it that hold one; where none does, it's solid or open as most of them are, by
// half the finest of their sides; open where none is held. An edge is crossed where the plane
// of the leaf holding it crosses it, or failing one, between the finest points along it, so
// coarse blocks and finer ones find the same crossing where they share it. Read by one thread.
class GroundField {
public:
    // The pages must outlive it.
    explicit GroundField(const PagesSnapshot& pages);

    [[nodiscard]] double value(I64Vec3 point);
    [[nodiscard]] Crossed crossing(I64Vec3 low, I64Vec3 high);
    // Reading this, which must outlive what's returned.
    [[nodiscard]] BlockField field();

private:
    struct Leaf {
        grid::Key node;
        grid::Surface value;
        bool held = false;
    };
    // At a point: its value, and the leaves round it that hold a plane, their mean normal and
    // what the nearest shows.
    struct Sample {
        double value = 0.0;
        DVec3 normal{0.0};
        std::uint16_t material = 0;
    };

    [[nodiscard]] DVec3 at(I64Vec3 point) const noexcept;
    [[nodiscard]] double distance(const Leaf& leaf, DVec3 point) const noexcept;
    // Of the finest node at `point`, its low corner.
    [[nodiscard]] const Leaf& leaf(I64Vec3 point);
    [[nodiscard]] bool holds(const Leaf& leaf, I64Vec3 point, bool inside) const noexcept;
    [[nodiscard]] Sample sample(I64Vec3 point);
    // The leaf holding all of the edge, if the finest nodes along it all lie in one.
    [[nodiscard]] const Leaf* holding(I64Vec3 low, I64Vec3 high);
    [[nodiscard]] static Crossed crossed(DVec3 point, const Sample& near, I64Vec3 low, I64Vec3 high,
                                         bool low_solid);

    const PagesSnapshot* pages_;
    grid::Grid grid_;
    DVec3 origin_;
    std::int64_t across_;                            // finest nodes along a side of the root
    std::unordered_map<std::uint64_t, Leaf> leaves_; // by the finest node's code
};

} // namespace hog::client
