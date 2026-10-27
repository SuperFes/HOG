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

[[nodiscard]] std::vector<std::uint16_t> coarse() {
    std::vector<std::uint16_t> indices;
    const auto vertex = [&](int i, int j) {
        indices.push_back(static_cast<std::uint16_t>((j * patch_side) + i));
    };
    for (int j = 0; j < last; j += coarse_step) {
        for (int i = 0; i < last; i += coarse_step) {
            const int i_next = i + coarse_step;
            const int j_next = j + coarse_step;
            vertex(i, j);
            vertex(i_next, j);
            vertex(i_next, j_next);
            vertex(i, j);
            vertex(i_next, j_next);
            vertex(i, j_next);
        }
    }
    return indices;
}

// With `step` overriding the steps along every edge, if it's given.
[[nodiscard]] std::vector<std::uint16_t> skirted_by(std::uint8_t coarser_edges, int step) {
    std::vector<std::uint16_t> indices;
    for (int edge = 0; edge < 4; ++edge) {
        // Along the edge, then down, faces out of the -u and +v edges and into the others.
        const bool forwards = edge == 0 || edge == 3;
        const auto triangle = [&](int a, int b, int c) {
            for (const int vertex : {a, forwards ? b : c, forwards ? c : b}) {
                indices.push_back(static_cast<std::uint16_t>(vertex));
            }
        };
        const auto top = [&](int along) {
            const GridPoint point = on_edge(edge, along);
            return (point.j * patch_side) + point.i;
        };
        const auto bottom = [&](int along) { return skirt_vertex(edge, along); };
        const int edge_step = ((coarser_edges >> edge) & 1U) != 0 ? 2 : 1;
        const int by = step != 0 ? step : edge_step;
        for (int along = 0; along < last; along += by) {
            triangle(top(along), top(along + by), bottom(along));
            triangle(top(along + by), bottom(along + by), bottom(along));
        }
    }
    return indices;
}

[[nodiscard]] std::vector<std::uint16_t> skirted(std::uint8_t coarser_edges) {
    return skirted_by(coarser_edges, 0);
}

template <auto Build>
[[nodiscard]] std::span<const std::uint16_t> built(std::uint8_t coarser_edges) {
    HOG_ASSERT(coarser_edges < stitchings, "stitching beyond a patch's four edges");
    static const auto sets = [] {
        std::array<std::vector<std::uint16_t>, stitchings> each;
        for (std::size_t edges = 0; edges < each.size(); ++edges) {
            each.at(edges) = Build(static_cast<std::uint8_t>(edges));
        }
        return each;
    }();
    return sets.at(coarser_edges);
}

} // namespace

std::span<const std::uint16_t> patch_indices(std::uint8_t coarser_edges) {
    return built<stitched>(coarser_edges);
}

std::span<const std::uint16_t> skirt_indices(std::uint8_t coarser_edges) {
    return built<skirted>(coarser_edges);
}

std::span<const std::uint16_t> coarse_patch_indices() {
    static const std::vector<std::uint16_t> indices = coarse();
    return indices;
}

std::span<const std::uint16_t> coarse_skirt_indices() {
    static const std::vector<std::uint16_t> indices = skirted_by(0, coarse_step);
    return indices;
}

} // namespace hog::terrain
