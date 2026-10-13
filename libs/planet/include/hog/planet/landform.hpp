#pragma once

#include <hog/core/math.hpp>
#include <hog/planet/cell_id.hpp>
#include <hog/planet/drainage.hpp>
#include <hog/planet/erosion.hpp>
#include <hog/planet/level_grid.hpp>

#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace hog::planet {

// Ground worn by rivers and raised by uplift over geological time, on a coarse grid: how far each
// grid cell moved, sampled smoothly anywhere on the planet.
class Landform {
public:
    // Evolves `heights`, fixed point and one per grid cell, through `steps` steps of `erosion`.
    Landform(LevelGrid grid, std::vector<std::int64_t> heights, const Erosion& erosion, int steps);

    [[nodiscard]] const LevelGrid& grid() const noexcept { return grid_; }
    // Fixed-point metres per grid cell, after evolving.
    [[nodiscard]] std::span<const std::int64_t> heights() const noexcept { return heights_; }
    // How the water runs over the evolved heights.
    [[nodiscard]] const Drainage& drainage() const noexcept { return drainage_; }
    // Square metres of ground draining through each grid cell, its own included, each weighted
    // by its runoff.
    [[nodiscard]] std::span<const std::int64_t> drained() const noexcept { return drained_; }

    // Fixed-point metres the ground moved, per grid cell.
    [[nodiscard]] std::span<const std::int64_t> change() const noexcept { return change_; }
    // The same at any cell's centre, bilinear between the four grid cells around it.
    [[nodiscard]] std::int64_t change(CellId cell) const;

private:
    // Grid cells around each one, by quadrant: the neighbour along u, along v and between the
    // two, for -u-v, +u-v, -u+v and +u+v. At a cube corner there is none between, and the third
    // is the cell itself.
    using Quadrant = std::array<std::uint32_t, 3>;

    LevelGrid grid_;
    std::vector<std::int64_t> heights_;
    std::vector<std::int64_t> change_;
    Drainage drainage_;
    std::vector<std::int64_t> drained_;
    std::vector<std::array<Quadrant, 4>> quadrants_;
};

// Smooth noise shifting points on the unit sphere by about a fifth of a grid cell, over bends a
// few cells long: sampling a grid at shifted points bends the straight lines between its cell
// centres into meanders.
class Meander {
public:
    Meander(std::uint64_t seed, const LevelGrid& grid);

    [[nodiscard]] DVec3 operator()(DVec3 point) const;

private:
    std::uint64_t seed_;
    double reach_;     // unit-sphere distance
    double frequency_; // per unit-sphere distance
};

} // namespace hog::planet
