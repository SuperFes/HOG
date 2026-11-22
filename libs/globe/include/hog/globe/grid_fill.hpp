#pragma once

#include <hog/globe/generator.hpp>
#include <hog/globe/placed.hpp>
#include <hog/grid/grid.hpp>
#include <hog/grid/key.hpp>
#include <hog/grid/tree.hpp>
#include <hog/grid/water_bodies.hpp>

#include <cstdint>
#include <vector>

namespace hog::globe {

// The grid round a generator's world: its root holding the sky's ceiling, halved down to the
// layers' thickness.
[[nodiscard]] grid::Grid grid_of(const Generator& generator);

// The palette entry of a material: the generator's own materials come first, as they are.
[[nodiscard]] constexpr grid::Material::Value palette_of(Material material) noexcept {
    return static_cast<grid::Material::Value>(material);
}

// The water tree's value for each body of water: the sea's 1, the lakes' after it by index, and
// the rivers' by the point their stem ends at, with the top bit set; none 0.
inline constexpr grid::Water::Value sea_body = 1;
inline constexpr grid::Water::Value river_bodies = 0x8000'0000U;
[[nodiscard]] constexpr grid::Water::Value body_value(WaterBody body) noexcept {
    switch (body.kind) {
    case WaterBody::Kind::Sea:
        return sea_body;
    case WaterBody::Kind::Lake:
        return sea_body + 1 + body.index;
    case WaterBody::Kind::River:
        return river_bodies | body.index;
    case WaterBody::Kind::None:
        break;
    }
    return 0;
}

// Each body of water the generator's water trees mark, by the value they mark it with: the sea at
// its level, each lake at its level, and each river along its main stem, its water as the finest
// detail shows it, with how each rises through the year.
[[nodiscard]] grid::WaterBodies water_bodies(const Generator& generator);

struct Filled {
    grid::Tree<grid::Shape> shape;
    grid::Tree<grid::Material> material;
    grid::Tree<grid::Water> water;
    grid::Tree<grid::Woods> woods;
    // What's placed apart from the ground's shape, its middle in the node.
    std::vector<Placed> placed;
};

// Where finer nodes are read from coarser ones with seeded layers: nodes narrower than `below`
// metres are read between the columns of the finest nodes no narrower, with octaves of noise for
// the detail finer than those, seeded by position, as strong as `strength` times what the columns
// say that detail could move the ground; none where `below` is 0. With `unique`, where a river's
// channel, a shore, a crag or a cave too narrow for those columns reaches, the generator's own
// reads, blended in over a column's width.
struct Layered {
    double below = 0.0;
    double strength = 1.0;
    bool unique = true;
};

// The trees over the node at `key`, `levels` deep under it, from the generator. Each node is
// sampled at its middle with detail no finer than itself, and keeps what it's sampled as whether
// it splits or not, so a node reads the same however deep it's filled and from whichever node
// over it; one the surface clearly misses, with no
// cave shown in it at that detail, is a leaf all solid or all open, so only nodes near the
// surface split, down to `levels`, where each holds the surface's plane. Material is the solid's
// just inside the surface; water marks the open space over the ground under the sea, a lake or a
// river, as high as it rises in the year, with the body it is; woods are those on the ground
// through each node, no finer than nodes 16 m across, none where no ground is. Leaves above
// the grid's finest depth are open where finer detail can still come: near the surface, and in
// solid where caves may lie. Fallen rocks big enough for the finest detail filled to show, and
// crags, are placed apart, left out of the shape, as are vents; smaller rocks lie spread as
// rubble in the ground.
// Precondition: key.depth + levels <= grid.depth().
[[nodiscard]] Filled fill(const Generator& generator, const grid::Grid& grid, grid::Key key,
                          std::uint8_t levels, Layered layered = {});

} // namespace hog::globe
