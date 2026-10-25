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

namespace hog::planet {

namespace {

constexpr std::uint64_t meander_salt = 0x6D65'616E'6465'7273ULL; // "meanders"
// Noise rarely strays far from zero: this shifts points by a fifth of a cell typically and at most
// three quarters, without ever folding the shifted grid over itself.
constexpr double meander_reach = 0.875;
constexpr double meander_cells = 4.0;
constexpr int meander_octaves = 2;

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
