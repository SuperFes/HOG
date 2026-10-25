#include <hog/core/assert.hpp>
#include <hog/core/hash.hpp>
#include <hog/planet/landform.hpp>
#include <hog/planet/neighbors.hpp>
#include <hog/planet/noise.hpp>
#include <hog/planet/projection.hpp>
#include <hog/planet/tile.hpp>

#include <algorithm>
#include <cstdlib>
#include <utility>
#include <vector>

namespace hog::planet {

namespace {

constexpr std::uint64_t meander_salt = 0x6D65'616E'6465'7273ULL; // "meanders"
// Noise rarely strays far from zero: this shifts points by a fifth of a cell typically and at most
// three quarters, without ever folding the shifted grid over itself.
constexpr double meander_reach = 0.875;
constexpr double meander_cells = 4.0;
constexpr int meander_octaves = 2;

// Ground this far under the filled surface lies in a basin, which takes in the ground about it
// filled at least a little deep: shallower is only a flat's slope towards its outlet.
constexpr std::int64_t basin_depth = fixed_one;
constexpr std::int64_t filled_depth = fixed_one / 100;
// What a lake loses a year to evaporation, net of the rain on it, against what a saturated
// catchment of its area runs off: a lake fills where its catchment, weighted by runoff, makes up
// for it, so basins in wet country hold lakes and those in deserts stay dry.
constexpr double evaporation = 0.4;

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
    std::vector<std::int64_t> rain(grid_.size());
    for (std::uint32_t cell = 0; cell < grid_.size(); ++cell) {
        rain[cell] = (grid_.area(cell) * erosion.runoff[cell]) >> fixed_fraction_bits;
    }
    drained_ = accumulate(drainage_, rain);
    change_.resize(heights_.size());
    for (std::size_t cell = 0; cell < heights_.size(); ++cell) {
        change_[cell] = heights_[cell] - start[cell];
    }
    find_lakes();
}

Landform::Landform(LevelGrid grid, std::vector<std::int64_t> heights,
                   std::vector<std::int64_t> change, std::vector<std::int64_t> drained)
    : grid_{std::move(grid)}, heights_{std::move(heights)}, change_{std::move(change)},
      drainage_{drain(grid_, heights_)}, drained_{std::move(drained)} {
    HOG_ASSERT(heights_.size() == grid_.size() && change_.size() == grid_.size() &&
                   drained_.size() == grid_.size(),
               "one value per grid cell");
    find_lakes();
}

void Landform::find_lakes() {
    const std::uint32_t count = grid_.size();
    const auto& surface = drainage_.surface;
    const auto filled = [&](std::uint32_t cell, std::int64_t depth) {
        return drainage_.receiver[cell] != cell && surface[cell] - heights_[cell] >= depth;
    };
    basins_.assign(count, false);
    lakes_.assign(count, dry);
    lakeside_.assign(count, 0);
    std::vector<std::uint32_t> cells;
    for (std::uint32_t start = 0; start < count; ++start) {
        if (basins_[start] || !filled(start, basin_depth)) {
            continue;
        }
        cells.assign(1, start);
        basins_[start] = true;
        for (std::size_t next = 0; next < cells.size(); ++next) {
            for (const std::uint32_t around : grid_.neighbors(cells[next])) {
                if (!basins_[around] && filled(around, filled_depth)) {
                    basins_[around] = true;
                    cells.push_back(around);
                }
            }
        }
        fill(cells);
    }
    // A point less than a cell from a centre is blended from that cell's neighbours and theirs.
    near_lakes_.assign(count, false);
    for (std::uint32_t cell = 0; cell < count; ++cell) {
        near_lakes_[cell] = lakeside_[cell] != 0;
    }
    for (int ring = 0; ring < 2; ++ring) {
        const std::vector<bool> before = near_lakes_;
        for (std::uint32_t cell = 0; cell < count; ++cell) {
            for (const std::uint32_t around : grid_.neighbors(cell)) {
                near_lakes_[around] = near_lakes_[around] || before[cell];
            }
        }
    }
}

void Landform::fill(std::span<const std::uint32_t> basin) {
    double area = 0.0;
    std::int64_t inflow = 0;
    std::int64_t level = dry;
    for (const std::uint32_t cell : basin) {
        area += static_cast<double>(grid_.area(cell));
        inflow = std::max(inflow, drained_[cell]);
        level = std::max(level, drainage_.surface[cell]);
    }
    if (static_cast<double>(inflow) < evaporation * area) {
        return;
    }
    for (const std::uint32_t cell : basin) {
        lakes_[cell] = level;
        lakeside_[cell] = fixed_one;
        for (const std::uint32_t around : grid_.neighbors(cell)) {
            lakeside_[around] = fixed_one;
        }
    }
}

bool Landform::near_lake(CellId cell) const {
    const int level = grid_.level();
    return near_lakes_[grid_.index(cell.level() >= level ? cell.parent(level)
                                                         : cell_at(cell_center(cell), level))];
}

std::int64_t Landform::change(CellId cell) const {
    return grid_.sample(change_, cell);
}

Meander::Meander(std::uint64_t seed, const LevelGrid& grid)
    : seed_{hash_combine(seed, meander_salt)} {
    const double spacing =
        glm::length(cell_center(grid.cell(0)) - cell_center(grid.cell(grid.neighbors(0)[0])));
    reach_ = meander_reach * spacing;
    frequency_ = 1.0 / (meander_cells * spacing);
}

DVec3 Meander::operator()(DVec3 point) const {
    const NoisePoint at = noise_point(point, frequency_);
    const DVec3 shift{fixed_to_double(fbm(seed_, at, meander_octaves)),
                      fixed_to_double(fbm(seed_ + 1, at, meander_octaves)),
                      fixed_to_double(fbm(seed_ + 2, at, meander_octaves))};
    return glm::normalize(point + (shift * reach_));
}

} // namespace hog::planet
