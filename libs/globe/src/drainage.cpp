#include <hog/core/assert.hpp>
#include <hog/globe/cell.hpp>
#include <hog/globe/drainage.hpp>

#include <algorithm>
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

    // Lowest first.
    friend bool operator<(const Waiting& a, const Waiting& b) noexcept {
        return a.height > b.height;
    }
};

} // namespace

Drainage drain(std::span<const float> ground, int level, double radius, double sea) {
    const std::size_t count = static_cast<std::size_t>(cells_at(level));
    HOG_ASSERT(ground.size() == count, "one ground height for each cell");
    constexpr std::uint32_t none = std::numeric_limits<std::uint32_t>::max();

    Drainage drained{.level = level,
                     .ground = {ground.begin(), ground.end()},
                     .filled = {ground.begin(), ground.end()},
                     .receiver = std::vector<std::uint32_t>(count, none),
                     .catchment = std::vector<double>(count)};

    std::priority_queue<Waiting> frontier;
    const auto outlet = [&](std::uint32_t cell) {
        drained.receiver[cell] = cell;
        frontier.push({drained.filled[cell], cell});
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
            frontier.push({drained.filled[other], other});
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
    return drained;
}

} // namespace hog::globe
