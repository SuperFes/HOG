#pragma once

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
    std::vector<std::array<Quadrant, 4>> quadrants_;
};

} // namespace hog::planet
