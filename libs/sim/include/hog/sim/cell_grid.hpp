#pragma once

#include <hog/core/math.hpp>
#include <hog/planet/cell_id.hpp>
#include <hog/planet/tangent_frame.hpp>

#include <array>
#include <cstdint>
#include <vector>

namespace hog::sim {

// Every cell of one cube-sphere level, numbered face by face and row by row, with the geometry
// per-cell simulations need between neighbours. Lengths are on the unit sphere, in radians.
class CellGrid {
public:
    static constexpr int max_level = 10;

    // Precondition: 0 <= level <= max_level.
    explicit CellGrid(int level);

    [[nodiscard]] int level() const noexcept { return level_; }
    [[nodiscard]] std::uint32_t size() const noexcept {
        return static_cast<std::uint32_t>(frames_.size());
    }

    [[nodiscard]] planet::CellId cell(std::uint32_t index) const;
    // Precondition: the cell is at the grid's level.
    [[nodiscard]] std::uint32_t index(planet::CellId cell) const;

    // The edge neighbours, in planet::edge_neighbors order.
    [[nodiscard]] const std::array<std::uint32_t, 4>& neighbors(std::uint32_t index) const {
        return neighbors_[index];
    }
    [[nodiscard]] const planet::TangentFrame& frame(std::uint32_t index) const {
        return frames_[index];
    }

    // Where each neighbour's centre lies from this cell's, as (east, north) radians.
    [[nodiscard]] const std::array<DVec2, 4>& offsets(std::uint32_t index) const {
        return offsets_[index];
    }

    // Steradians, summing to 4π over the grid.
    [[nodiscard]] double area(std::uint32_t index) const { return areas_[index]; }

    // A field's (east, north) gradient per radian is the sum of weights[k] times the field's
    // difference from this cell to neighbour k.
    [[nodiscard]] const std::array<DVec2, 4>& gradient_weights(std::uint32_t index) const {
        return gradient_weights_[index];
    }

private:
    int level_;
    std::uint32_t side_;
    std::vector<planet::TangentFrame> frames_;
    std::vector<double> areas_;
    std::vector<std::array<std::uint32_t, 4>> neighbors_;
    std::vector<std::array<DVec2, 4>> offsets_;
    std::vector<std::array<DVec2, 4>> gradient_weights_;
};

} // namespace hog::sim
