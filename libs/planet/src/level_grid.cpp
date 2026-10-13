#include <hog/core/assert.hpp>
#include <hog/planet/level_grid.hpp>
#include <hog/planet/neighbors.hpp>
#include <hog/planet/noise.hpp>
#include <hog/planet/projection.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace hog::planet {

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
    distances_.reserve(std::size_t{count} * 8);
    for (std::uint32_t index = 0; index < count; ++index) {
        first_.push_back(static_cast<std::uint32_t>(around_.size()));
        const VertexNeighbors near = vertex_neighbors(cell(index));
        for (const CellId next : near.cells()) {
            const std::uint32_t there = this->index(next);
            around_.push_back(there);
            // Chords, not arcs: no trigonometry, and the difference at these sizes is tiny.
            const double chord = glm::length(centers[there] - centers[index]) * radius;
            distances_.push_back(std::llround(chord * static_cast<double>(fixed_one)));
        }
    }
    first_.push_back(static_cast<std::uint32_t>(around_.size()));
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

} // namespace hog::planet
