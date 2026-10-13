#include <hog/core/assert.hpp>
#include <hog/planet/noise.hpp>
#include <hog/planet/projection.hpp>
#include <hog/planet/rivers.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>
#include <vector>

namespace hog::planet {

namespace {

// A metre of rain a year runs off the rain-weighted area.
constexpr double seconds_per_year = 31'557'600.0;

// Banks rise from the water at this slope until they meet the ground.
constexpr double bank_slope = 0.5;

double flow(double area) {
    return area / seconds_per_year;
}

std::int64_t fixed(double metres) {
    return std::llround(metres * static_cast<double>(fixed_one));
}

constexpr std::int64_t no_lake = std::numeric_limits<std::int64_t>::min();
// Ground this far under the landform's filled surface lies in a basin.
constexpr std::int64_t basin_depth = fixed_one;
// What a lake loses a year to evaporation, net of the rain on it, against what a saturated
// catchment of its area runs off: a lake fills where its catchment, weighted by runoff, makes up
// for it, so basins in wet country hold lakes and those in deserts stay dry.
constexpr double evaporation = 0.4;

struct Basins {
    std::vector<bool> basin;         // per landform cell
    std::vector<std::int64_t> lakes; // the level of the lake filling it, or no_lake when dry
};

// Groups of neighbouring cells deep under the filled surface: lakes where enough water comes in,
// dry basins where it doesn't.
Basins find_basins(const Landform& landform) {
    const LevelGrid& grid = landform.grid();
    const auto heights = landform.heights();
    const auto& surface = landform.drainage().surface;
    const auto drained = landform.drained();
    const std::uint32_t count = grid.size();
    const auto deep = [&](std::uint32_t cell) {
        return landform.drainage().receiver[cell] != cell &&
               surface[cell] - heights[cell] >= basin_depth;
    };
    Basins found{.basin = std::vector<bool>(count, false),
                 .lakes = std::vector<std::int64_t>(count, no_lake)};
    std::vector<std::uint32_t> cells;
    for (std::uint32_t start = 0; start < count; ++start) {
        if (found.basin[start] || !deep(start)) {
            continue;
        }
        cells.assign(1, start);
        found.basin[start] = true;
        for (std::size_t next = 0; next < cells.size(); ++next) {
            for (const std::uint32_t around : grid.neighbors(cells[next])) {
                if (!found.basin[around] && deep(around)) {
                    found.basin[around] = true;
                    cells.push_back(around);
                }
            }
        }
        double area = 0.0;
        std::int64_t inflow = 0;
        std::int64_t level = no_lake;
        for (const std::uint32_t cell : cells) {
            area += static_cast<double>(grid.area(cell));
            inflow = std::max(inflow, drained[cell]);
            level = std::max(level, surface[cell]);
        }
        if (static_cast<double>(inflow) >= evaporation * area) {
            for (const std::uint32_t cell : cells) {
                found.lakes[cell] = level;
            }
        }
    }
    return found;
}

} // namespace

double Rivers::width(double area) {
    return 2.5 * std::sqrt(flow(area));
}

double Rivers::depth(double area) {
    return 0.5 * std::sqrt(std::sqrt(flow(area)));
}

Rivers::Rivers(const Landform& landform, std::span<const std::int64_t> ground, Meander meander,
               double radius, double scale)
    : grid_{&landform.grid()}, meander_{meander}, radius_{radius} {
    const LevelGrid& grid = landform.grid();
    const Drainage& drainage = landform.drainage();
    const auto drained = landform.drained();
    const std::uint32_t count = grid.size();
    HOG_ASSERT(ground.size() == count, "one ground height per landform cell");

    Basins basins = find_basins(landform);
    lakes_ = std::move(basins.lakes);
    const auto outlet = [&](std::uint32_t cell) { return drainage.receiver[cell] == cell; };
    const auto lake = [&](std::uint32_t cell) { return lakes_[cell] != no_lake; };
    // Rivers stop at a lake's shore, and leave from its outlet; dry basins hold none.
    const auto flows = [&](std::uint32_t cell) {
        return !outlet(cell) && !basins.basin[cell] &&
               static_cast<double>(drained[cell]) >= source_area;
    };
    // A river's water stands at the ground of its cell's centre, but never above the water
    // upstream, so it only falls towards the sea and cuts through rises on the way.
    levels_.assign(ground.begin(), ground.end());
    for (auto cell = drainage.order.rbegin(); cell != drainage.order.rend(); ++cell) {
        const std::uint32_t receiver = drainage.receiver[*cell];
        if (flows(*cell) && flows(receiver)) {
            levels_[receiver] = std::min(levels_[receiver], levels_[*cell]);
        }
    }
    // Lakes are level. Rivers stand no lower than the lake they run into, and ground away from
    // rivers drains into the water downstream: a river, a lake or the sea, or the lowest ground
    // of a world without one.
    for (const std::uint32_t cell : drainage.order) {
        const std::uint32_t receiver = drainage.receiver[cell];
        if (outlet(cell)) {
            levels_[cell] = std::max<std::int64_t>(ground[cell], 0);
        } else if (lake(cell)) {
            levels_[cell] = lakes_[cell];
        } else if (flows(cell)) {
            levels_[cell] = std::max(levels_[cell], levels_[receiver]);
        } else {
            levels_[cell] = levels_[receiver];
        }
    }

    std::vector<std::pair<std::uint32_t, std::uint32_t>> near;
    for (std::uint32_t cell = 0; cell < count; ++cell) {
        if (!flows(cell)) {
            continue;
        }
        const std::uint32_t receiver = drainage.receiver[cell];
        const auto area = static_cast<double>(drained[cell]);
        const auto reach = static_cast<std::uint32_t>(reaches_.size());
        reaches_.push_back({.from = cell_center(grid.cell(cell)),
                            .to = cell_center(grid.cell(receiver)),
                            .from_surface = levels_[cell],
                            .to_surface = levels_[receiver],
                            .width = scale * width(area),
                            .depth = fixed(scale * depth(area))});
        for (const std::uint32_t end : {cell, receiver}) {
            near.emplace_back(end, reach);
            for (const std::uint32_t next : grid.neighbors(end)) {
                near.emplace_back(next, reach);
            }
        }
    }
    std::ranges::sort(near);
    const auto [last, end] = std::ranges::unique(near);
    near.erase(last, end);
    first_.assign(count + 1, 0);
    nearby_.reserve(near.size());
    for (const auto& [cell, reach] : near) {
        ++first_[cell + 1];
        nearby_.push_back(reach);
    }
    for (std::uint32_t cell = 0; cell < count; ++cell) {
        first_[cell + 1] += first_[cell];
    }
}

template <typename Visit>
void Rivers::each_reach(CellId cell, Visit visit) const {
    // The reaches are found from the shifted point, which lies on the river's line.
    const DVec3 point = meander_(cell_center(cell));
    const std::uint32_t here = grid_->index(cell_at(point, grid_->level()));
    for (std::uint32_t k = first_[here]; k < first_[here + 1]; ++k) {
        const Reach& reach = reaches_[nearby_[k]];
        const DVec3 along = reach.to - reach.from;
        const double t =
            std::clamp(glm::dot(point - reach.from, along) / glm::dot(along, along), 0.0, 1.0);
        // From the great circle through the ends, not the chord, which dips below the ground.
        double away = 0.0;
        if (t <= 0.0) {
            away = glm::length(point - reach.from);
        } else if (t >= 1.0) {
            away = glm::length(point - reach.to);
        } else {
            away = std::abs(glm::dot(point, glm::normalize(glm::cross(reach.from, reach.to))));
        }
        const std::int64_t level =
            reach.from_surface +
            std::llround(static_cast<double>(reach.to_surface - reach.from_surface) * t);
        visit(reach, level, away * radius_);
    }
}

std::int64_t Rivers::cut(CellId cell, std::int64_t ground) const {
    std::int64_t lowest = ground;
    each_reach(cell, [&](const Reach& reach, std::int64_t level, double away) {
        // Where the ground dips below the river's line, the water follows it down.
        const std::int64_t surface = std::min(level, ground);
        const double half = reach.width / 2.0;
        std::int64_t bed = 0;
        if (away < half) {
            const double share = 1.0 - ((away / half) * (away / half));
            bed = surface - std::llround(static_cast<double>(reach.depth) * share);
        } else {
            bed =
                surface + std::llround((away - half) * bank_slope * static_cast<double>(fixed_one));
        }
        lowest = std::min(lowest, bed);
    });
    return lowest;
}

std::optional<Channel> Rivers::channel(CellId cell, std::int64_t ground) const {
    std::optional<std::int64_t> surface;
    each_reach(cell, [&](const Reach& reach, std::int64_t level, double away) {
        if (away < reach.width / 2.0) {
            surface = std::min({surface.value_or(level), level, ground});
        }
    });
    if (!surface) {
        return std::nullopt;
    }
    return Channel{.bed = cut(cell, ground), .surface = *surface};
}

std::optional<std::int64_t> Rivers::lake(CellId cell, std::int64_t ground) const {
    // The highest lake about the shifted point, so shores follow the ground, not the grid.
    const std::uint32_t here = grid_->index(cell_at(meander_(cell_center(cell)), grid_->level()));
    std::int64_t level = lakes_[here];
    for (const std::uint32_t around : grid_->neighbors(here)) {
        level = std::max(level, lakes_[around]);
    }
    if (level == no_lake || ground >= level) {
        return std::nullopt;
    }
    return level;
}

const Reach* Rivers::near(CellId cell, double margin) const {
    const Reach* widest = nullptr;
    each_reach(cell, [&](const Reach& reach, std::int64_t /*level*/, double away) {
        if (away < (reach.width / 2.0) + margin &&
            (widest == nullptr || reach.width > widest->width)) {
            widest = &reach;
        }
    });
    return widest;
}

} // namespace hog::planet
