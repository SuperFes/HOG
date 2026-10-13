#include <hog/core/assert.hpp>
#include <hog/planet/landform.hpp>
#include <hog/planet/neighbors.hpp>
#include <hog/planet/noise.hpp>

#include <algorithm>
#include <cstdlib>
#include <utility>

namespace hog::planet {

namespace {

// The cell other than `cell` touching both `along_u` and `along_v`, or `cell` where the three
// meet at a cube corner.
std::uint32_t between(const LevelGrid& grid, std::uint32_t cell, std::uint32_t along_u,
                      std::uint32_t along_v) {
    const auto edges = edge_neighbors(grid.cell(along_u));
    if (std::ranges::contains(edges, grid.cell(along_v))) {
        return cell;
    }
    for (const std::uint32_t candidate : grid.neighbors(along_u)) {
        if (candidate != cell && std::ranges::contains(grid.neighbors(along_v), candidate)) {
            return candidate;
        }
    }
    HOG_ASSERT(false, "two cells beside one share another neighbour");
    return cell;
}

std::int64_t lerp(std::int64_t from, std::int64_t to, std::int64_t weight) {
    return from + (((to - from) * weight) >> fixed_fraction_bits);
}

} // namespace

Landform::Landform(LevelGrid grid, std::vector<std::int64_t> heights, const Erosion& erosion,
                   int steps)
    : grid_{std::move(grid)}, heights_{std::move(heights)} {
    HOG_ASSERT(heights_.size() == grid_.size(), "one height per grid cell");
    const std::vector<std::int64_t> start = heights_;
    for (int step = 0; step < steps; ++step) {
        std::ignore = erode(grid_, heights_, erosion);
    }
    drainage_ = drain(grid_, heights_);
    change_.resize(heights_.size());
    for (std::size_t cell = 0; cell < heights_.size(); ++cell) {
        change_[cell] = heights_[cell] - start[cell];
    }

    quadrants_.resize(grid_.size());
    for (std::uint32_t cell = 0; cell < grid_.size(); ++cell) {
        const auto edges = edge_neighbors(grid_.cell(cell));
        for (std::size_t quadrant = 0; quadrant < 4; ++quadrant) {
            const std::uint32_t along_u = grid_.index(edges[quadrant % 2]);
            const std::uint32_t along_v = grid_.index(edges[2 + (quadrant / 2)]);
            quadrants_[cell][quadrant] = {along_u, along_v, between(grid_, cell, along_u, along_v)};
        }
    }
}

std::int64_t Landform::change(CellId cell) const {
    const int level = grid_.level();
    const FaceIJ at = cell.to_face_ij();
    // Centres in units of 2^-31 of a face, so a cell's centre is a whole number at any level.
    const auto centre = [&](std::uint32_t i, int of_level) {
        return ((std::int64_t{i} * 2) + 1) << (max_level - of_level);
    };
    const std::int64_t u = centre(at.i, at.level);
    const std::int64_t v = centre(at.j, at.level);
    const int shift = max_level + 1 - level;
    const auto coarse_i = static_cast<std::uint32_t>(u >> shift);
    const auto coarse_j = static_cast<std::uint32_t>(v >> shift);
    const std::int64_t du = u - centre(coarse_i, level);
    const std::int64_t dv = v - centre(coarse_j, level);
    const std::int64_t wu = (std::abs(du) << fixed_fraction_bits) >> shift;
    const std::int64_t wv = (std::abs(dv) << fixed_fraction_bits) >> shift;

    const std::uint32_t here =
        grid_.index(CellId::from_face_ij(at.face, coarse_i, coarse_j, level));
    const Quadrant& around = quadrants_[here][(du < 0 ? 0U : 1U) + (dv < 0 ? 0U : 2U)];
    const std::int64_t along_u = change_[around[0]];
    const std::int64_t along_v = change_[around[1]];
    // Where three cells meet at a cube corner, the missing fourth takes their mean, so the corner
    // gets the same value from each.
    const std::int64_t diagonal =
        around[2] == here ? (change_[here] + along_u + along_v) / 3 : change_[around[2]];
    return lerp(lerp(change_[here], along_u, wu), lerp(along_v, diagonal, wu), wv);
}

} // namespace hog::planet
