#pragma once

#include <hog/core/math.hpp>
#include <hog/globe/globe.hpp>
#include <hog/terrain/blocks.hpp>

namespace hog::client {

// How a hexel shows as a block: its material's colour, or molten rock's glow the hotter it is.
[[nodiscard]] Vec3 hexel_colour(const globe::Hexel& hexel);

// The globe's hexels as blocks, from its centre.
[[nodiscard]] terrain::BlockMesh globe_blocks(const globe::Globe& globe);

} // namespace hog::client
