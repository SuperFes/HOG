#pragma once

#include <hog/core/math.hpp>
#include <hog/planet/cell_id.hpp>
#include <hog/planet/drainage.hpp>
#include <hog/planet/erosion.hpp>
#include <hog/planet/hillslope.hpp>
#include <hog/planet/level_grid.hpp>

#include <array>
#include <cstdint>
#include <functional>
#include <limits>
#include <span>
#include <vector>

namespace hog::planet {

// Ground worn by rivers and raised by uplift over geological time, on a coarse grid: how far each
// grid cell moved, sampled smoothly anywhere on the planet. Basins in it hold lakes where enough
// water runs in to make up for what evaporates, and stay dry where it doesn't.
class Landform {
public:
    // A cell's lake level where it holds none.
    static constexpr std::int64_t dry = std::numeric_limits<std::int64_t>::min();

    // Evolves `heights`, fixed point and one per grid cell, through `steps` steps of `erosion`
    // and of `hillslope`, if it has fields, calling `stepped` with how many are done after each.
    Landform(LevelGrid grid, std::vector<std::int64_t> heights, const Erosion& erosion,
             const Hillslope& hillslope, int steps, const std::function<void(int)>& stepped = {});
    // One evolved before, as its heights, change, drained, soil and risen left it, by erosion
    // with `wander`.
    Landform(LevelGrid grid, std::vector<std::int64_t> heights, std::vector<std::int64_t> change,
             std::vector<std::int64_t> drained, std::vector<std::int64_t> soil,
             std::vector<std::int64_t> risen, std::uint64_t wander);

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
    // The same at any cell's centre, smoothly, so the worn ground shows no creases along the
    // grid cells' edges.
    [[nodiscard]] std::int64_t change(CellId cell) const;
    // Fixed-point metres of soil on each grid cell: thick where it has weathered and gathered,
    // none on bare rock.
    [[nodiscard]] std::span<const std::int64_t> soil() const noexcept { return soil_; }
    // Fixed-point metres each grid cell was lifted while it stood above the sea: the ground less
    // this is the height the rock lay at before, where its beds are found.
    [[nodiscard]] std::span<const std::int64_t> risen() const noexcept { return risen_; }

    // Whether a grid cell lies in a basin, holding a lake or dry.
    [[nodiscard]] bool basin(std::uint32_t index) const { return basins_[index]; }
    // The fixed-point level of the lake over each grid cell, or `dry`. A lake takes in all the
    // ground under its level about it, so the cells around one stand above it.
    [[nodiscard]] std::span<const std::int64_t> lakes() const noexcept { return lakes_; }
    // Fixed-point 1 at cells holding lakes and those beside them, 0 elsewhere: sampled anywhere,
    // it is 1 wherever a lake's cells are blended into the ground and fades out a cell beyond.
    [[nodiscard]] std::span<const std::int64_t> lakeside() const noexcept { return lakeside_; }
    // Whether lakeside may be above 0 anywhere less than a grid cell from a cell's centre.
    [[nodiscard]] bool near_lake(CellId cell) const;

private:
    void find_lakes();
    // Fills a basin's cells with a lake if enough water runs in.
    void fill(std::span<const std::uint32_t> basin);

    LevelGrid grid_;
    std::vector<std::int64_t> heights_;
    std::vector<std::int64_t> change_;
    std::vector<std::int64_t> soil_;
    std::vector<std::int64_t> risen_;
    Drainage drainage_;
    std::vector<std::int64_t> drained_;
    std::vector<bool> basins_;
    std::vector<std::int64_t> lakes_;
    std::vector<std::int64_t> lakeside_;
    std::vector<bool> near_lakes_;
};

// Smooth noise shifting points on the unit sphere by about a fifth of a grid cell, over bends a
// few cells long, or as on 500 m cells where the grid is finer: sampling a grid at shifted points
// bends the straight lines between its cell centres into meanders.
class Meander {
public:
    // `radius` is the planet's, in metres.
    Meander(std::uint64_t seed, const LevelGrid& grid, double radius);

    [[nodiscard]] DVec3 operator()(DVec3 point) const;

private:
    std::uint64_t seed_;
    double reach_;     // unit-sphere distance
    double frequency_; // per unit-sphere distance
};

} // namespace hog::planet
