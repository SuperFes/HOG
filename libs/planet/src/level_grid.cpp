#include <hog/core/assert.hpp>
#include <hog/planet/level_grid.hpp>
#include <hog/planet/neighbors.hpp>
#include <hog/planet/noise.hpp>
#include <hog/planet/projection.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
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

LevelGrid::LevelGrid(int level, double radius)
    : level_(level), side_(std::uint32_t{1} << std::clamp(level, 0, max_level)) {
    HOG_ASSERT(level >= 0 && level <= max_level, "grid level out of range");
    HOG_ASSERT(radius > 0.0, "radius must be positive");
    const std::uint32_t count = face_count * side_ * side_;
    std::vector<DVec3> centers;
    centers.reserve(count);
    areas_.reserve(count);
    for (std::uint32_t index = 0; index < count; ++index) {
        centers.push_back(cell_center(cell(index)));
        // The flat quad through the corners: half the cross product of its diagonals.
        const auto v = cell_vertices(cell(index));
        const double flat = 0.5 * glm::length(glm::cross(v[2] - v[0], v[3] - v[1]));
        areas_.push_back(std::llround(flat * radius * radius));
    }
    first_.reserve(count + 1);
    around_.reserve(std::size_t{count} * 8);
    std::vector<std::int64_t> distances;
    distances.reserve(std::size_t{count} * 8);
    for (std::uint32_t index = 0; index < count; ++index) {
        first_.push_back(static_cast<std::uint32_t>(around_.size()));
        const VertexNeighbors near = vertex_neighbors(cell(index));
        for (const CellId next : near.cells()) {
            const std::uint32_t there = this->index(next);
            around_.push_back(there);
            // Chords, not arcs: no trigonometry, and the difference at these sizes is tiny.
            const double chord = glm::length(centers[there] - centers[index]) * radius;
            distances.push_back(std::llround(chord * static_cast<double>(fixed_one)));
        }
    }
    first_.push_back(static_cast<std::uint32_t>(around_.size()));
    const std::int64_t longest = std::ranges::max(distances);
    while (!std::in_range<std::uint32_t>(longest >> distance_shift_)) {
        ++distance_shift_;
    }
    distances_.reserve(distances.size());
    for (const std::int64_t distance : distances) {
        distances_.push_back(static_cast<std::uint32_t>(distance >> distance_shift_));
    }

    for (std::uint32_t index = 0; index < count; ++index) {
        const std::uint32_t on_face = index % (side_ * side_);
        const std::uint32_t i = on_face % side_;
        const std::uint32_t j = on_face / side_;
        if (i != 0 && j != 0 && i + 1 != side_ && j + 1 != side_) {
            continue;
        }
        const auto edges = edge_neighbors(cell(index));
        std::array<Quadrant, 4> around{};
        for (std::size_t quadrant = 0; quadrant < 4; ++quadrant) {
            const std::uint32_t along_u = this->index(edges[quadrant % 2]);
            const std::uint32_t along_v = this->index(edges[2 + (quadrant / 2)]);
            around.at(quadrant) = {along_u, along_v, between(*this, index, along_u, along_v)};
        }
        edge_cells_.push_back(index);
        edge_quadrants_.push_back(around);
    }
}

LevelGrid::Quadrant LevelGrid::quadrant(std::uint32_t here, std::size_t quadrant) const {
    const auto edge = std::ranges::lower_bound(edge_cells_, here);
    if (edge != edge_cells_.end() && *edge == here) {
        return edge_quadrants_[static_cast<std::size_t>(edge - edge_cells_.begin())].at(quadrant);
    }
    const std::uint32_t along_u = quadrant % 2 == 0 ? here - 1 : here + 1;
    const std::uint32_t across = quadrant / 2 == 0 ? here - side_ : here + side_;
    return {along_u, across, along_u + across - here};
}

CellId LevelGrid::cell(std::uint32_t index) const {
    const std::uint32_t per_face = side_ * side_;
    HOG_ASSERT(index < face_count * per_face, "cell index out of range");
    const std::uint32_t on_face = index % per_face;
    return CellId::from_face_ij(static_cast<int>(index / per_face), on_face % side_,
                                on_face / side_, level_);
}

std::uint32_t LevelGrid::index(CellId cell) const {
    const FaceIJ at = cell.to_face_ij();
    HOG_ASSERT(at.level == level_, "cell is not at the grid's level");
    return (static_cast<std::uint32_t>(at.face) * side_ * side_) + (at.j * side_) + at.i;
}

LevelGrid::Blend LevelGrid::blend(CellId cell) const {
    const int level = level_;
    const FaceIJ at = cell.to_face_ij();
    // Centres in units of 2^-31 of a face, so a cell's centre is a whole number at any level.
    const auto centre = [&](std::uint32_t i, int of_level) {
        return ((std::int64_t{i} * 2) + 1) << (planet::max_level - of_level);
    };
    const std::int64_t u = centre(at.i, at.level);
    const std::int64_t v = centre(at.j, at.level);
    const int shift = planet::max_level + 1 - level;
    const auto coarse_i = static_cast<std::uint32_t>(u >> shift);
    const auto coarse_j = static_cast<std::uint32_t>(v >> shift);
    const std::int64_t du = u - centre(coarse_i, level);
    const std::int64_t dv = v - centre(coarse_j, level);

    const std::uint32_t here = index(CellId::from_face_ij(at.face, coarse_i, coarse_j, level));
    const Quadrant around = quadrant(here, (du < 0 ? 0U : 1U) + (dv < 0 ? 0U : 2U));
    return {.cells = {here, around[0], around[1], around[2]},
            .along_u = (std::abs(du) << fixed_fraction_bits) >> shift,
            .along_v = (std::abs(dv) << fixed_fraction_bits) >> shift};
}

std::int64_t LevelGrid::sample(std::span<const std::int64_t> field, CellId cell) const {
    HOG_ASSERT(field.size() == size(), "one value per grid cell");
    const auto [cells, wu, wv] = blend(cell);
    const auto [here, along_u, along_v, between] = cells;
    // Where three cells meet at a cube corner, the missing fourth takes their mean, so the corner
    // gets the same value from each.
    const std::int64_t diagonal =
        between == here ? (field[here] + field[along_u] + field[along_v]) / 3 : field[between];
    return lerp(lerp(field[here], field[along_u], wu), lerp(field[along_v], diagonal, wu), wv);
}

std::array<std::uint32_t, 4> LevelGrid::corners(CellId cell) const {
    return blend(cell).cells;
}

} // namespace hog::planet
