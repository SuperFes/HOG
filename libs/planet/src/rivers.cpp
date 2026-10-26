#include <hog/core/assert.hpp>
#include <hog/planet/noise.hpp>
#include <hog/planet/projection.hpp>
#include <hog/planet/rivers.hpp>

#include <algorithm>
#include <cmath>
#include <utility>
#include <vector>

namespace hog::planet {

namespace {

// How far from a river's edge its banks may climb, at most: some widths across, and never
// beyond a third of its reach's length from its middle, which the cells it's found from always
// cover.
constexpr double bank_fade = 20.0;
constexpr double bank_reach = 1.0 / 3.0;
// A metre of rain a year runs off the rain-weighted area.
constexpr double seconds_per_year = 31'557'600.0;

double flow(double area) {
    return area / seconds_per_year;
}

std::int64_t fixed(double metres) {
    return std::llround(metres * static_cast<double>(fixed_one));
}

// The sea over landform cells, and the cells below the sea's level but cut off from it.
struct Standing {
    std::vector<bool> sea;
    std::vector<bool> sunken;
};

Standing find_standing(const Landform& landform) {
    const std::uint32_t count = landform.grid().size();
    Standing found{.sea = std::vector<bool>(count, false),
                   .sunken = std::vector<bool>(count, false)};
    for (std::uint32_t cell = 0; cell < count; ++cell) {
        if (landform.heights()[cell] > 0) {
            continue;
        }
        // Only the sea's own cells drain nowhere.
        if (landform.drainage().receiver[cell] == cell) {
            found.sea[cell] = true;
        } else {
            found.sunken[cell] = true;
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
    : landform_{&landform}, grid_{&landform.grid()}, meander_{meander}, radius_{radius} {
    const LevelGrid& grid = landform.grid();
    const Drainage& drainage = landform.drainage();
    const auto drained = landform.drained();
    const std::uint32_t count = grid.size();
    HOG_ASSERT(ground.size() == count, "one ground height per landform cell");

    const auto lakes = landform.lakes();
    const auto outlet = [&](std::uint32_t cell) { return drainage.receiver[cell] == cell; };
    const auto lake = [&](std::uint32_t cell) { return lakes[cell] != Landform::dry; };
    Standing standing = find_standing(landform);
    sea_ = std::move(standing.sea);
    sunken_ = std::move(standing.sunken);
    // Rivers stop at a lake's shore, and leave from its outlet; dry basins hold none.
    const auto flows = [&](std::uint32_t cell) {
        return !outlet(cell) && !landform.basin(cell) &&
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
            levels_[cell] = lakes[cell];
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

std::int64_t Rivers::sunk(const Reach& reach, std::int64_t level, std::int64_t ground) const {
    // Reaches are only found from the cells about their ends, so the banks must climb back to
    // the ground well within them; where the ground stands higher, so does the river in it.
    const double half = reach.width / 2.0;
    const double banks =
        std::max(std::min(bank_fade * reach.width,
                          bank_reach * glm::length(reach.to - reach.from) * radius_) -
                     half,
                 0.0);
    const std::int64_t deepest = std::llround(banks * bank_slope * static_cast<double>(fixed_one));
    // Where the ground dips below the river's line, the water follows it down.
    return std::max(std::min(level, ground), ground - deepest);
}

std::int64_t Rivers::cut(CellId cell, std::int64_t ground) const {
    std::int64_t lowest = ground;
    each_reach(cell, [&](const Reach& reach, std::int64_t level, double away) {
        const std::int64_t surface = sunk(reach, level, ground);
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
            const std::int64_t water = sunk(reach, level, ground);
            surface = std::min(surface.value_or(water), water);
        }
    });
    if (!surface) {
        return std::nullopt;
    }
    return Channel{.bed = cut(cell, ground), .surface = *surface};
}

std::optional<StandingWater> Rivers::standing(CellId cell, std::int64_t ground) const {
    // Found about the shifted point, so shores follow the ground, not the grid.
    const CellId shifted = cell_at(meander_(cell_center(cell)));
    // A lake fills the ground under its level wherever its cells are blended into it; the cells
    // round it stand above its level, so the water meets the ground all along its shore.
    std::int64_t lake = Landform::dry;
    for (const std::uint32_t corner : grid_->corners(shifted)) {
        lake = std::max(lake, landform_->lakes()[corner]);
    }
    if (lake != Landform::dry) {
        if (ground < lake) {
            return StandingWater{.level = lake, .sea = false};
        }
        return std::nullopt;
    }
    // The sea stands over its own cells and those beside them, as low as the ground lies.
    if (ground >= 0) {
        return std::nullopt;
    }
    const std::uint32_t here = grid_->index(shifted.parent(grid_->level()));
    bool sea = sea_[here];
    bool sunken = sunken_[here];
    for (const std::uint32_t around : grid_->neighbors(here)) {
        sea = sea || sea_[around];
        sunken = sunken || sunken_[around];
    }
    // Low ground finer than the grid takes the sea, unless it lies by ground cut off from it.
    if (sea || !sunken) {
        return StandingWater{.level = 0, .sea = true};
    }
    return std::nullopt;
}

std::optional<std::int64_t> Rivers::lake(CellId cell, std::int64_t ground) const {
    const auto water = standing(cell, ground);
    if (!water || water->sea) {
        return std::nullopt;
    }
    return water->level;
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
