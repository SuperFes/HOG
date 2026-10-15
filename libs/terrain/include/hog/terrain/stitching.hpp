#pragma once

#include <cstdint>
#include <span>

namespace hog::terrain {

// Edges of a patch, in edge_neighbors' order, as bits of a stitching.
inline constexpr std::uint8_t edge_minus_u = 1U << 0U;
inline constexpr std::uint8_t edge_plus_u = 1U << 1U;
inline constexpr std::uint8_t edge_minus_v = 1U << 2U;
inline constexpr std::uint8_t edge_plus_v = 1U << 3U;
inline constexpr int stitchings = 16;

// Triangles over a patch's vertices, three indices each, counter-clockwise seen from above. Along
// the edges in `coarser_edges` the border's odd vertices are left out, so those edges run
// straight between the vertices a neighbour a level coarser has there.
[[nodiscard]] std::span<const std::uint16_t> patch_indices(std::uint8_t coarser_edges);

// Triangles over a patch's skirts, facing out of it, hung from patch_indices' outline for the
// same edges.
[[nodiscard]] std::span<const std::uint16_t> skirt_indices(std::uint8_t coarser_edges);

} // namespace hog::terrain
