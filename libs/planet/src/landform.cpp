#include <hog/core/assert.hpp>
#include <hog/core/hash.hpp>
#include <hog/planet/glaciers.hpp>
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
// On fine grids the bends keep to the size they'd have on cells this wide, metres, so rivers still
// wander visibly.
constexpr double meander_spacing = 500.0;
constexpr int meander_octaves = 2;

constexpr std::uint64_t settled_salt = 0x7365'7474'6C65'6400ULL; // "settled"

// Keys the way water runs over the evolved ground, apart from every step's.
std::uint64_t settled(std::uint64_t wander) {
    return wander == 0 ? 0 : hash_combine(wander, settled_salt);
}

// Ground this far under the filled surface lies in a basin, which takes in the ground about it
// filled at least a little deep: shallower is only a flat's slope towards its outlet.
constexpr std::int64_t basin_depth = fixed_one;
constexpr std::int64_t filled_depth = fixed_one / 100;
// What a lake loses a year to evaporation, net of the rain on it, against what a saturated
// catchment of its area runs off: a lake fills where its catchment, weighted by runoff, makes up
// for it, so basins in wet country hold lakes and those in deserts stay dry.
constexpr double evaporation = 0.4;

// Fixed-point metres of glacial cut over which a trough takes in the change of its ground.
constexpr std::int64_t trough_blend = 20 * fixed_one;

} // namespace

Landform::Landform(LevelGrid grid, std::vector<std::int64_t> heights, const Erosion& erosion,
                   const Hillslope& hillslope, int steps, const std::function<void(int)>& stepped,
                   const Glaciation& glaciation)
    : grid_{std::move(grid)}, heights_{std::move(heights)}, soil_(heights_.size(), 0),
      risen_(heights_.size(), 0), carved_(heights_.size(), 0) {
    HOG_ASSERT(heights_.size() == grid_.size(), "one height per grid cell");
    const std::vector<std::int64_t> start = heights_;
    for (int step = 0; step < steps; ++step) {
        Erosion now = erosion;
        now.risen = risen_;
        // A new way down each step, so no one path is worn in by chance alone.
        now.wander =
            erosion.wander == 0 ? 0 : hash_values(erosion.wander, static_cast<std::uint64_t>(step));
        const std::vector<std::int64_t> before = heights_;
        std::ignore = erode(grid_, heights_, now);
        if (!hillslope.creep.empty()) {
            // Rivers carry off the soil before cutting into the rock.
            for (std::size_t cell = 0; cell < heights_.size(); ++cell) {
                const std::int64_t rose = before[cell] > 0 ? erosion.uplift[cell] : 0;
                const std::int64_t cut = before[cell] + rose - heights_[cell];
                soil_[cell] = std::max<std::int64_t>(soil_[cell] - cut, 0);
            }
            Hillslope slopes = hillslope;
            slopes.risen = risen_;
            wear_slopes(grid_, heights_, soil_, slopes);
        }
        if (stepped) {
            stepped(step + 1);
        }
    }
    change_.resize(heights_.size());
    for (std::size_t cell = 0; cell < heights_.size(); ++cell) {
        change_[cell] = heights_[cell] - start[cell];
    }
    if (glaciation.balance) {
        const std::vector<std::int64_t> cut = glaciate(grid_, heights_, glaciation);
        for (std::size_t cell = 0; cell < heights_.size(); ++cell) {
            heights_[cell] -= cut[cell];
            // The ice scours the soil off as it cuts.
            soil_[cell] = std::max<std::int64_t>(soil_[cell] - cut[cell], 0);
            // A trough takes in the change of the ground it was cut from, so its floor stands
            // where the ice left it, wherever the coast falls in it.
            const std::int64_t taken =
                (change_[cell] *
                 ((std::min(cut[cell], trough_blend) << fixed_fraction_bits) / trough_blend)) >>
                fixed_fraction_bits;
            change_[cell] -= taken;
            carved_[cell] = cut[cell] - taken;
        }
    }
    drainage_ = drain(grid_, heights_, 0, settled(erosion.wander));
    std::vector<std::int64_t> rain(grid_.size());
    for (std::uint32_t cell = 0; cell < grid_.size(); ++cell) {
        rain[cell] = (grid_.area(cell) * erosion.runoff[cell]) >> fixed_fraction_bits;
    }
    drained_ = accumulate(drainage_, rain);
    find_lakes();
    find_steepness();
}

Landform::Landform(LevelGrid grid, std::vector<std::int64_t> heights,
                   std::vector<std::int64_t> change, std::vector<std::int64_t> drained,
                   std::vector<std::int64_t> soil, std::vector<std::int64_t> risen,
                   std::vector<std::int64_t> carved, std::uint64_t wander)
    : grid_{std::move(grid)}, heights_{std::move(heights)}, change_{std::move(change)},
      soil_{std::move(soil)}, risen_{std::move(risen)}, carved_{std::move(carved)},
      drainage_{drain(grid_, heights_, 0, settled(wander))}, drained_{std::move(drained)} {
    HOG_ASSERT(heights_.size() == grid_.size() && change_.size() == grid_.size() &&
                   drained_.size() == grid_.size() && soil_.size() == grid_.size() &&
                   risen_.size() == grid_.size() && carved_.size() == grid_.size(),
               "one value per grid cell");
    find_lakes();
    find_steepness();
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
    find_lakes_about();
}

void Landform::find_lakes_about() {
    const std::uint32_t count = grid_.size();
    submerged_.assign(count, 0);
    lakes_about_ = lakes_;
    for (std::uint32_t cell = 0; cell < count; ++cell) {
        if (lakes_[cell] == dry) {
            continue;
        }
        submerged_[cell] = fixed_one;
        for (const std::uint32_t around : grid_.neighbors(cell)) {
            lakes_about_[around] = std::max(lakes_about_[around], lakes_[cell]);
        }
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

void Landform::find_steepness() {
    steepness_.assign(grid_.size(), 0);
    for (std::uint32_t cell = 0; cell < grid_.size(); ++cell) {
        const auto around = grid_.neighbors(cell);
        for (std::size_t k = 0; k < around.size(); ++k) {
            const std::int64_t rise = std::abs(heights_[cell] - heights_[around[k]]);
            steepness_[cell] =
                std::max(steepness_[cell], (rise << fixed_fraction_bits) /
                                               std::max<std::int64_t>(grid_.distance(cell, k), 1));
        }
    }
}

bool Landform::near_lake(CellId cell) const {
    const int level = grid_.level();
    return near_lakes_[grid_.index(cell.level() >= level ? cell.parent(level)
                                                         : cell_at(cell_center(cell), level))];
}

std::int64_t Landform::change(CellId cell) const {
    return grid_.smooth(change_, cell);
}

Meander::Meander(std::uint64_t seed, const LevelGrid& grid, double radius)
    : seed_{hash_combine(seed, meander_salt)} {
    const double spacing = std::max(
        glm::length(cell_center(grid.cell(0)) - cell_center(grid.cell(grid.neighbors(0)[0]))),
        meander_spacing / radius);
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
