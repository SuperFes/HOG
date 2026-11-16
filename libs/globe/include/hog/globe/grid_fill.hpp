#pragma once

#include <hog/globe/generator.hpp>
#include <hog/grid/grid.hpp>
#include <hog/grid/key.hpp>
#include <hog/grid/tree.hpp>

#include <cstdint>

namespace hog::globe {

// The grid round a generator's world: its root holding the sky's ceiling, halved down to the
// layers' thickness.
[[nodiscard]] grid::Grid grid_of(const Generator& generator);

// The palette entry of a material: the generator's own materials come first, as they are.
[[nodiscard]] constexpr grid::Material::Value palette_of(Material material) noexcept {
    return static_cast<grid::Material::Value>(material);
}

// The water body the sea is, until bodies are made from the generator's water.
inline constexpr grid::Water::Value sea_body = 1;

struct Filled {
    grid::Tree<grid::Shape> shape;
    grid::Tree<grid::Material> material;
    grid::Tree<grid::Water> water;
};

// The trees over the node at `key`, `levels` deep under it, from the generator. Each node is
// sampled at its middle with detail no finer than itself; one the surface clearly misses is a
// leaf all solid or all open, so only nodes near the surface split, down to `levels`, where each
// holds the surface's plane. Material is the solid's just inside the surface; water marks the
// open space under the sea. Leaves near the surface above the grid's finest depth are open, as
// finer detail can still come there. Precondition: key.depth + levels <= grid.depth().
[[nodiscard]] Filled fill(const Generator& generator, const grid::Grid& grid, grid::Key key,
                          std::uint8_t levels);

} // namespace hog::globe
