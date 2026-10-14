#include <hog/core/assert.hpp>
#include <hog/terrain/patch.hpp>
#include <hog/terrain/stitching.hpp>

#include <array>
#include <cstddef>
#include <utility>
#include <vector>

namespace hog::terrain {

namespace {

constexpr int last = patch_cells;

struct GridPoint {
    int i;
    int j;
};

// `along` an edge and `in` rows from it.
[[nodiscard]] GridPoint on_edge(int edge, int along, int in) {
    switch (edge) {
    case 0:
        return {.i = in, .j = along};
    case 1:
        return {.i = last - in, .j = along};
    case 2:
        return {.i = along, .j = in};
    default:
        return {.i = along, .j = last - in};
    }
}

[[nodiscard]] std::vector<std::uint16_t> stitched(std::uint8_t coarser_edges) {
    std::vector<std::uint16_t> indices;
    const auto triangle = [&](GridPoint a, GridPoint b, GridPoint c) {
        if (((b.i - a.i) * (c.j - a.j)) - ((b.j - a.j) * (c.i - a.i)) < 0) {
            std::swap(b, c);
        }
        for (const GridPoint point : {a, b, c}) {
            indices.push_back(static_cast<std::uint16_t>((point.j * patch_side) + point.i));
        }
    };
    // Cells inside the ring along the border.
    for (int j = 1; j < last - 1; ++j) {
        for (int i = 1; i < last - 1; ++i) {
            const GridPoint low{.i = i, .j = j};
            const GridPoint high{.i = i + 1, .j = j + 1};
            triangle(low, {.i = i + 1, .j = j}, high);
            triangle(low, high, {.i = i, .j = j + 1});
        }
    }
    // The ring, an edge at a time: from the border to the row inside it, which stops a step
    // short of either end where the next edge's part of the ring takes over.
    for (int edge = 0; edge < 4; ++edge) {
        const auto border = [&](int along) { return on_edge(edge, along, 0); };
        const auto inner = [&](int along) { return on_edge(edge, along, 1); };
        if (((coarser_edges >> edge) & 1U) != 0) {
            for (int along = 0; along < last; along += 2) {
                triangle(border(along), border(along + 2), inner(along + 1));
                if (along > 0) {
                    triangle(border(along), inner(along - 1), inner(along));
                    triangle(border(along), inner(along), inner(along + 1));
                }
            }
            continue;
        }
        triangle(border(0), border(1), inner(1));
        for (int along = 1; along < last - 1; ++along) {
            triangle(border(along), border(along + 1), inner(along + 1));
            triangle(border(along), inner(along + 1), inner(along));
        }
        triangle(border(last - 1), border(last), inner(last - 1));
    }
    return indices;
}

} // namespace

std::span<const std::uint16_t> patch_indices(std::uint8_t coarser_edges) {
    HOG_ASSERT(coarser_edges < stitchings, "stitching beyond a patch's four edges");
    static const auto sets = [] {
        std::array<std::vector<std::uint16_t>, stitchings> built;
        for (std::size_t edges = 0; edges < built.size(); ++edges) {
            built.at(edges) = stitched(static_cast<std::uint8_t>(edges));
        }
        return built;
    }();
    return sets.at(coarser_edges);
}

} // namespace hog::terrain
