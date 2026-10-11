#include <hog/core/assert.hpp>
#include <hog/planet/neighbors.hpp>
#include <hog/planet/projection.hpp>
#include <hog/sim/cell_grid.hpp>

#include <algorithm>
#include <cmath>

namespace hog::sim {

using planet::CellId;

CellGrid::CellGrid(int level)
    : level_(level), side_(std::uint32_t{1} << std::clamp(level, 0, max_level)) {
    HOG_ASSERT(level >= 0 && level <= max_level, "grid level out of range");
    const std::uint32_t count = planet::face_count * side_ * side_;
    frames_.reserve(count);
    neighbors_.reserve(count);
    gradient_weights_.reserve(count);
    for (std::uint32_t index = 0; index < count; ++index) {
        frames_.push_back(planet::tangent_frame(cell(index)));
    }
    for (std::uint32_t index = 0; index < count; ++index) {
        const planet::TangentFrame& here = frames_[index];
        const auto cells = planet::edge_neighbors(cell(index));
        std::array<std::uint32_t, 4> around{};
        std::array<DVec2, 4> offsets{};
        // Least squares over the four neighbours: gradient = (sum d d^T)^-1 sum d * difference.
        double xx = 0.0;
        double xy = 0.0;
        double yy = 0.0;
        for (std::size_t k = 0; k < cells.size(); ++k) {
            around[k] = this->index(cells[k]);
            const DVec3 there = frames_[around[k]].up;
            const DVec2 heading{glm::dot(there, here.east), glm::dot(there, here.north)};
            const double arc = std::acos(std::clamp(glm::dot(there, here.up), -1.0, 1.0));
            offsets[k] = glm::normalize(heading) * arc;
            xx += offsets[k].x * offsets[k].x;
            xy += offsets[k].x * offsets[k].y;
            yy += offsets[k].y * offsets[k].y;
        }
        const double determinant = (xx * yy) - (xy * xy);
        std::array<DVec2, 4> weights{};
        for (std::size_t k = 0; k < offsets.size(); ++k) {
            weights[k] = DVec2{(yy * offsets[k].x) - (xy * offsets[k].y),
                               (xx * offsets[k].y) - (xy * offsets[k].x)} /
                         determinant;
        }
        neighbors_.push_back(around);
        gradient_weights_.push_back(weights);
    }
}

CellId CellGrid::cell(std::uint32_t index) const {
    const std::uint32_t per_face = side_ * side_;
    HOG_ASSERT(index < planet::face_count * per_face, "cell index out of range");
    const std::uint32_t on_face = index % per_face;
    return CellId::from_face_ij(static_cast<int>(index / per_face), on_face % side_,
                                on_face / side_, level_);
}

std::uint32_t CellGrid::index(CellId cell) const {
    const planet::FaceIJ at = cell.to_face_ij();
    HOG_ASSERT(at.level == level_, "cell is not at the grid's level");
    return (static_cast<std::uint32_t>(at.face) * side_ * side_) + (at.j * side_) + at.i;
}

} // namespace hog::sim
