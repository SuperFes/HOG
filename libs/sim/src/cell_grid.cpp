#include <hog/core/assert.hpp>
#include <hog/planet/neighbors.hpp>
#include <hog/planet/projection.hpp>
#include <hog/sim/cell_grid.hpp>

#include <algorithm>
#include <cmath>

namespace hog::sim {

using planet::CellId;

namespace {

double arc(DVec3 from, DVec3 to) {
    return std::acos(std::clamp(glm::dot(from, to), -1.0, 1.0));
}

} // namespace

CellGrid::CellGrid(int level)
    : level_(level), side_(std::uint32_t{1} << std::clamp(level, 0, max_level)) {
    HOG_ASSERT(level >= 0 && level <= max_level, "grid level out of range");
    const std::uint32_t count = planet::face_count * side_ * side_;
    frames_.reserve(count);
    areas_.reserve(count);
    neighbors_.reserve(count);
    offsets_.reserve(count);
    gradient_weights_.reserve(count);
    laplacian_weights_.reserve(count);
    for (std::uint32_t index = 0; index < count; ++index) {
        frames_.push_back(planet::tangent_frame(cell(index)));
        areas_.push_back(planet::cell_area(cell(index)));
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
            offsets[k] = glm::normalize(heading) * arc(here.up, there);
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
        offsets_.push_back(offsets);
        gradient_weights_.push_back(weights);
        laplacian_weights_.push_back(laplacian(index, around));
    }
}

std::array<double, 4> CellGrid::laplacian(std::uint32_t index,
                                          const std::array<std::uint32_t, 4>& around) const {
    const auto corners = planet::cell_vertices(cell(index));
    std::array<double, 4> weights{};
    for (std::size_t k = 0; k < around.size(); ++k) {
        const DVec3 there = frames_[around[k]].up;
        // The shared edge runs between the two corners nearest the neighbour.
        std::array<DVec3, 4> nearest = corners;
        std::ranges::sort(
            nearest, [&](DVec3 a, DVec3 b) { return glm::dot(a, there) > glm::dot(b, there); });
        weights[k] = arc(nearest[0], nearest[1]) / (arc(frames_[index].up, there) * areas_[index]);
    }
    return weights;
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

CellGrid::Shares CellGrid::shares(DVec3 point) const {
    const std::uint32_t cell = index(planet::cell_at(point, level_));
    Shares around;
    around.cells = {neighbors_[cell][0], neighbors_[cell][1], neighbors_[cell][2],
                    neighbors_[cell][3], cell};
    const planet::TangentFrame& here = frames_[cell];
    const DVec2 heading{glm::dot(point, here.east), glm::dot(point, here.north)};
    const double length = glm::length(heading);
    if (length == 0.0) {
        around.weights.back() = 1.0;
        return around;
    }
    const double arc = std::atan2(length, glm::dot(point, here.up));
    const DVec2 offset = heading * (arc / length);
    double toward = 0.0;
    for (std::size_t k = 0; k < 4; ++k) {
        const DVec2 to = offsets_[cell][k];
        const double share = std::clamp(glm::dot(offset, to) / glm::dot(to, to), 0.0, 1.0);
        around.weights.at(k) = share;
        toward += share;
    }
    const double stay = std::max(1.0 - toward, 0.0);
    around.weights.back() = stay;
    around.total = toward + stay;
    return around;
}

} // namespace hog::sim
