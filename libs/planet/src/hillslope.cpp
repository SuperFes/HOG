#include <hog/core/assert.hpp>
#include <hog/planet/hillslope.hpp>
#include <hog/planet/noise.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <span>

namespace hog::planet {

namespace {

// Metres of soil over which weathering slows to half.
constexpr double weathering_depth = 0.5;
// Metres of soil over which creep comes to half its pace; thinner soil holds on to the rock.
constexpr double creeping_depth = 0.5;
// How near the steepest slope creep is reckoned, so it speeds up only so far in one step.
constexpr double nearest_steepest = 0.85;
// The nine-point Laplacian's weights across an edge and a corner.
constexpr double edge_weight = 2.0 / 3.0;
constexpr double corner_weight = 1.0 / 3.0;
// Neighbours this much further than the nearest are across a corner.
constexpr double corner_beyond = 1.2;
// Loose ground this deep stands no steeper than scree lies.
constexpr std::int64_t loose_depth = fixed_one;
constexpr double repose = 0.7;
// Slopes this little steeper than the rock holds are left to creep.
constexpr std::int64_t collapse_tolerance = fixed_one / 100;
// Falls handled per cell in a step, at most.
constexpr std::size_t collapse_rounds = 64;

[[nodiscard]] double metres(std::int64_t fixed) {
    return fixed_to_double(fixed);
}

[[nodiscard]] std::int64_t fixed(double metres) {
    return std::llround(metres * static_cast<double>(fixed_one));
}

} // namespace

namespace {

// Weathers the rock under each land cell's soil into more of it, returning how steep each
// cell's rock stands.
std::vector<double> weather(const LevelGrid& grid, const std::vector<std::int64_t>& heights,
                            std::vector<std::int64_t>& soil, const Hillslope& hillslope,
                            std::int64_t sea_level) {
    std::vector<double> steepest(grid.size(), 1.0);
    for (std::uint32_t cell = 0; cell < grid.size(); ++cell) {
        Weathering rock;
        if (hillslope.rock) {
            const std::int64_t risen = hillslope.risen.empty() ? 0 : hillslope.risen[cell];
            rock = hillslope.rock(cell, heights[cell] - soil[cell] - risen);
        }
        // Read under the sea too, as fallen rock may build the ground up out of it.
        steepest[cell] = metres(rock.steepest);
        if (soil[cell] > loose_depth) {
            steepest[cell] = std::min(steepest[cell], repose);
        }
        if (heights[cell] <= sea_level) {
            continue;
        }
        // Soil thickening as dh/dt = P h0 / (h0 + h), integrated over the step.
        const double rate = metres(hillslope.weathering[cell]) * metres(rock.rate);
        const double was = weathering_depth + metres(soil[cell]);
        const double now = std::sqrt((was * was) + (2.0 * rate * weathering_depth));
        soil[cell] += fixed(now - was);
    }
    return steepest;
}

// What each land cell sheds to its lower neighbours.
class Shedding {
public:
    Shedding(const LevelGrid& grid, const std::vector<std::int64_t>& heights,
             const std::vector<std::int64_t>& soil, const std::vector<double>& steepest,
             std::span<const std::int64_t> creep)
        : grid_{&grid}, heights_{heights}, soil_{soil}, steepest_{steepest}, creep_{creep} {}

    // Metres of ground a cell sheds to each lower neighbour, before being held to its soil.
    template <typename Visit>
    void each(std::uint32_t cell, Visit visit) const {
        const auto around = grid_->neighbors(cell);
        std::int64_t closest = grid_->distance(cell, 0);
        for (std::size_t k = 1; k < around.size(); ++k) {
            closest = std::min(closest, grid_->distance(cell, k));
        }
        const double nearest = metres(closest);
        const double thick = metres(soil_[cell]);
        const double moving = metres(creep_[cell]) * (thick / (thick + creeping_depth));
        for (std::size_t k = 0; k < around.size(); ++k) {
            const std::uint32_t lower = around[k];
            if (heights_[lower] >= heights_[cell]) {
                continue;
            }
            const double distance = metres(grid_->distance(cell, k));
            const double drop = metres(heights_[cell] - heights_[lower]);
            const double ratio = std::min(drop / distance / steepest_[cell], nearest_steepest);
            const double weight = distance > nearest * corner_beyond ? corner_weight : edge_weight;
            visit(lower, weight * moving * drop / (distance * distance) / (1.0 - (ratio * ratio)));
        }
    }

private:
    const LevelGrid* grid_;
    std::span<const std::int64_t> heights_;
    std::span<const std::int64_t> soil_;
    std::span<const double> steepest_;
    std::span<const std::int64_t> creep_;
};

// Soil creeps from each land cell to those below it, never more than there is.
void creep(const LevelGrid& grid, std::vector<std::int64_t>& heights,
           std::vector<std::int64_t>& soil, const std::vector<double>& steepest,
           std::span<const std::int64_t> creeping, std::int64_t sea_level) {
    const std::uint32_t count = grid.size();
    const Shedding shedding{grid, heights, soil, steepest, creeping};
    const auto sheds = [&](std::uint32_t cell) {
        return heights[cell] > sea_level && soil[cell] > 0;
    };

    // Never more than the soil there is: each cell's shedding is scaled to fit it.
    std::vector<double> held(count, 0.0);
    for (std::uint32_t cell = 0; cell < count; ++cell) {
        if (!sheds(cell)) {
            continue;
        }
        double total = 0.0;
        shedding.each(cell, [&](std::uint32_t, double moved) { total += moved; });
        const double thick = metres(soil[cell]);
        held[cell] = total > thick ? thick / total : 1.0;
    }
    std::vector<double> change(count, 0.0);
    for (std::uint32_t cell = 0; cell < count; ++cell) {
        if (!sheds(cell)) {
            continue;
        }
        const auto area = static_cast<double>(grid.area(cell));
        shedding.each(cell, [&](std::uint32_t lower, double moved) {
            change[cell] -= moved * held[cell];
            // Spread over the lower cell's area, so no soil is made or lost.
            change[lower] += moved * held[cell] * area / static_cast<double>(grid.area(lower));
        });
    }
    for (std::uint32_t cell = 0; cell < count; ++cell) {
        const std::int64_t moved = fixed(change[cell]);
        heights[cell] += moved;
        soil[cell] = std::max<std::int64_t>(soil[cell] + moved, 0);
    }
}

// Cells steeper than their rock holds give way to those below until none is, the fallen rock
// lying loose on them.
void collapse(const LevelGrid& grid, std::vector<std::int64_t>& heights,
              std::vector<std::int64_t>& soil, const std::vector<double>& steepest,
              std::int64_t sea_level) {
    const std::uint32_t count = grid.size();
    std::vector<std::uint32_t> waiting(count);
    for (std::uint32_t cell = 0; cell < count; ++cell) {
        waiting[cell] = cell;
    }
    std::vector<bool> queued(count, true);
    // A bound on the work, should slopes ever pass the fall back and forth.
    const std::size_t most = static_cast<std::size_t>(count) * collapse_rounds;
    for (std::size_t next = 0; next < waiting.size() && next < most; ++next) {
        const std::uint32_t cell = waiting[next];
        queued[cell] = false;
        if (heights[cell] <= sea_level) {
            continue;
        }
        const auto around = grid.neighbors(cell);
        bool fell = false;
        for (std::size_t k = 0; k < around.size(); ++k) {
            const std::uint32_t lower = around[k];
            const std::int64_t holds = fixed(steepest[cell] * metres(grid.distance(cell, k)));
            const std::int64_t excess = heights[cell] - heights[lower] - holds;
            if (excess <= collapse_tolerance) {
                continue;
            }
            // Fall until the slope is as steep as the rock holds, the rock spread over the
            // lower cell's area.
            const double spread =
                static_cast<double>(grid.area(cell)) / static_cast<double>(grid.area(lower));
            const std::int64_t fallen = std::llround(static_cast<double>(excess) / (1.0 + spread));
            const std::int64_t landed = std::llround(static_cast<double>(fallen) * spread);
            heights[cell] -= fallen;
            soil[cell] = std::max<std::int64_t>(soil[cell] - fallen, 0);
            heights[lower] += landed;
            soil[lower] += landed;
            fell = true;
            if (!queued[lower]) {
                queued[lower] = true;
                waiting.push_back(lower);
            }
        }
        if (fell) {
            // Ground above may now stand too steep over this cell.
            for (const std::uint32_t above : around) {
                if (!queued[above]) {
                    queued[above] = true;
                    waiting.push_back(above);
                }
            }
        }
    }
}

} // namespace

void wear_slopes(const LevelGrid& grid, std::vector<std::int64_t>& heights,
                 std::vector<std::int64_t>& soil, const Hillslope& hillslope,
                 std::int64_t sea_level) {
    const std::uint32_t count = grid.size();
    HOG_ASSERT(heights.size() == count && soil.size() == count && hillslope.creep.size() == count &&
                   hillslope.weathering.size() == count &&
                   (hillslope.risen.empty() || hillslope.risen.size() == count),
               "one value per cell");
    const std::vector<double> steepest = weather(grid, heights, soil, hillslope, sea_level);
    collapse(grid, heights, soil, steepest, sea_level);
    creep(grid, heights, soil, steepest, hillslope.creep, sea_level);
}

} // namespace hog::planet
