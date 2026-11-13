#include <hog/core/assert.hpp>
#include <hog/globe/cell.hpp>
#include <hog/globe/drainage.hpp>
#include <hog/globe/maths.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <queue>
#include <ranges>
#include <utility>

namespace hog::globe {

namespace {

// Level ground gets this much fall per cell, so water crossing a flat or a full hollow keeps
// going to the rim rather than stalling.
constexpr float flat_fall = 1.0e-3F;

struct Waiting {
    float height;
    std::uint32_t cell;

    // Lowest first; ties by cell, as standard libraries order equal keys differently.
    friend bool operator<(const Waiting& a, const Waiting& b) noexcept {
        return a.height != b.height ? a.height > b.height : a.cell > b.cell;
    }
};

} // namespace

Drainage drain(std::span<const float> ground, int level, double radius, double sea) {
    const auto count = static_cast<std::size_t>(cells_at(level));
    HOG_ASSERT(ground.size() == count, "one ground height for each cell");
    constexpr std::uint32_t none = std::numeric_limits<std::uint32_t>::max();

    Drainage drained{.level = level,
                     .ground = {ground.begin(), ground.end()},
                     .filled = {ground.begin(), ground.end()},
                     .receiver = std::vector<std::uint32_t>(count, none),
                     .catchment = std::vector<double>(count),
                     .order = {}};

    std::priority_queue<Waiting> frontier;
    const auto outlet = [&](std::uint32_t cell) {
        drained.receiver[cell] = cell;
        frontier.push({.height = drained.filled[cell], .cell = cell});
    };
    bool any_sea = false;
    for (std::size_t cell = 0; cell < count; ++cell) {
        if (static_cast<double>(ground[cell]) <= sea) {
            outlet(static_cast<std::uint32_t>(cell));
            any_sea = true;
        }
    }
    if (!any_sea) {
        outlet(static_cast<std::uint32_t>(std::ranges::min_element(ground) - ground.begin()));
    }

    std::vector<std::uint32_t> order; // the order cells were finished, upstream last
    order.reserve(count);
    while (!frontier.empty()) {
        const std::uint32_t at = frontier.top().cell;
        frontier.pop();
        order.push_back(at);
        const CellNeighbors around_at = around(cell_at_index(level, at));
        for (const Cell& next : around_at.all()) {
            const auto other = static_cast<std::uint32_t>(index_of(next));
            if (drained.receiver[other] != none) {
                continue;
            }
            drained.receiver[other] = at;
            drained.filled[other] = std::max(drained.filled[other], drained.filled[at] + flat_fall);
            frontier.push({.height = drained.filled[other], .cell = other});
        }
    }

    const double area_scale = radius * radius;
    for (std::size_t cell = 0; cell < count; ++cell) {
        drained.catchment[cell] =
            solid_angle(cell_at_index(level, static_cast<std::uint64_t>(cell))) * area_scale;
    }
    // Upstream first: each cell hands what drains through it to the one it runs on to.
    for (const std::uint32_t cell : std::views::reverse(order)) {
        if (drained.receiver[cell] != cell) {
            drained.catchment[drained.receiver[cell]] += drained.catchment[cell];
        }
    }
    drained.order = std::move(order);
    return drained;
}

std::vector<float> erode(std::span<const float> ground, int level, double radius, double sea,
                         const Erosion& erosion) {
    std::vector<float> now{ground.begin(), ground.end()};
    for (int step = 0; step < erosion.steps; ++step) {
        const Drainage drained = drain(now, level, radius, sea);
        // Downstream first, so each cell is cut toward a receiver that has already been.
        for (const std::uint32_t cell : drained.order) {
            const std::uint32_t receiver = drained.receiver[cell];
            if (receiver == cell || drained.depth(cell) > 0.0) {
                continue;
            }
            const double apart = radius * glm::length(center(cell_at_index(level, cell)) -
                                                      center(cell_at_index(level, receiver)));
            // A receiver holding water is cut toward by its surface, not its bed.
            const double target = drained.depth(receiver) > 0.0
                                      ? static_cast<double>(drained.filled[receiver])
                                      : static_cast<double>(now[receiver]);
            const double power =
                erosion.rate * maths::pow(drained.catchment[cell], erosion.area_power) / apart;
            const double lowered =
                (static_cast<double>(now[cell]) + (power * target)) / (1.0 + power);
            now[cell] = static_cast<float>(std::min(static_cast<double>(now[cell]), lowered));
        }
    }
    return now;
}

} // namespace hog::globe
