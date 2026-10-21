#pragma once

#include <hog/core/math.hpp>
#include <hog/planet/cell_id.hpp>
#include <hog/planet/projection.hpp>
#include <hog/planet/tangent_frame.hpp>

#include <algorithm>
#include <array>
#include <cmath>
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

    // A field's Laplacian per square radian is the sum of weights[k] times the field's difference
    // from this cell to neighbour k: the flux across each shared edge, over the cell's area, so
    // what flows out of one cell flows into its neighbour.
    [[nodiscard]] const std::array<double, 4>& laplacian_weights(std::uint32_t index) const {
        return laplacian_weights_[index];
    }

    // A field's value at a unit direction, where value(index) gives it at a cell: interpolated
    // linearly from the cell under the point towards each neighbour, by how far the point lies
    // in that neighbour's direction. The weights are never negative, so it never makes new
    // highs or lows.
    template <typename Value>
    [[nodiscard]] double interpolate(DVec3 point, Value value) const;

    // The cells interpolate() reads at a point, each neighbour then the cell under it, with how
    // much each counts, out of `total`.
    struct Shares {
        std::array<std::uint32_t, 5> cells{};
        std::array<double, 5> weights{};
        double total = 1.0;
    };
    [[nodiscard]] Shares shares(DVec3 point) const;

private:
    [[nodiscard]] std::array<double, 4> laplacian(std::uint32_t index,
                                                  const std::array<std::uint32_t, 4>& around) const;

    int level_;
    std::uint32_t side_;
    std::vector<planet::TangentFrame> frames_;
    std::vector<double> areas_;
    std::vector<std::array<std::uint32_t, 4>> neighbors_;
    std::vector<std::array<DVec2, 4>> offsets_;
    std::vector<std::array<DVec2, 4>> gradient_weights_;
    std::vector<std::array<double, 4>> laplacian_weights_;
};

template <typename Value>
double CellGrid::interpolate(DVec3 point, Value value) const {
    const Shares around = shares(point);
    double total = 0.0;
    for (std::size_t k = 0; k < around.cells.size(); ++k) {
        total += around.weights.at(k) * value(around.cells.at(k));
    }
    return total / around.total;
}

} // namespace hog::sim
