#pragma once

#include <hog/globe/voxel_mesh.hpp>
#include <hog/terrain/block_mesh.hpp>

#include <cstdint>
#include <optional>

namespace hog::client {

// A tile's faces as drawn: its ground, and its water still in lakes and pools.
struct Drawn {
    terrain::BlockMesh ground;
    terrain::BlockMesh water;
};

// What marks the sea's own surfaces among still water: the corner byte of each of their vertices.
inline constexpr std::uint8_t sea_corner = 1;
// What marks a water vertex on its sheet's edge, which waves and the tide may lower, never raise.
inline constexpr std::uint8_t shore_corner = 2;

// The faces split into ground and water: a liquid's faces at `sea` metres from the planet's
// centre, within a voxel `thickness`, are the sea's own, marked so for its waves to be drawn.
[[nodiscard]] Drawn drawn_of(const globe::VoxelMesh& mesh, std::optional<double> sea,
                             double thickness);

} // namespace hog::client
