#pragma once

#include <array>
#include <cstdint>

// Eric Lengyel's Transvoxel tables (MIT). A regular cell's corner i lies at (i & 1, i >> 1 & 1,
// i >> 2 & 1) and its case has bit i set where that corner is solid. A transition cell's samples
// 0 to 8 lie on its full face, row by row, 9 to 12 on its half face copying 0, 2, 6 and 8; its case
// takes them in the order 0, 1, 2, 5, 8, 7, 6, 3, 4. A vertex's low byte names the two corners or
// samples of its edge, a nibble each.
namespace hog::client::transvoxel {

struct RegularCell {
    std::uint8_t counts = 0; // vertices in the high nibble, triangles in the low
    std::array<std::uint8_t, 15> indices{};
};

struct TransitionCell {
    std::uint8_t counts = 0; // vertices in the high nibble, triangles in the low
    std::array<std::uint8_t, 36> indices{};
};

extern const std::array<std::uint8_t, 256> regular_cell_class;
extern const std::array<RegularCell, 16> regular_cell_data;
extern const std::array<std::array<std::uint16_t, 12>, 256> regular_vertex_data;
// Its high bit set where the triangles wind the other way.
extern const std::array<std::uint8_t, 512> transition_cell_class;
extern const std::array<TransitionCell, 56> transition_cell_data;
extern const std::array<std::array<std::uint16_t, 12>, 512> transition_vertex_data;

} // namespace hog::client::transvoxel
