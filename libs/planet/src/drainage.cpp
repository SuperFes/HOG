#include <hog/core/assert.hpp>
#include <hog/planet/drainage.hpp>
#include <hog/planet/noise.hpp>

#include <algorithm>
#include <cstddef>
#include <functional>
#include <queue>
#include <tuple>
#include <utility>

namespace hog::planet {

namespace {

// Lowest surface first; ties go to the cell reached first, so the order never depends on how
// the standard library builds its heap.
using Entry = std::tuple<std::int64_t, std::uint64_t, std::uint32_t>;
using Frontier = std::priority_queue<Entry, std::vector<Entry>, std::greater<>>;

// The largest stretch of neighbouring cells at or below `sea_level`, the first found of equals.
// Lower ground cut off from it is only a basin, holding a lake if enough water comes in.
std::vector<std::uint32_t> sea(const LevelGrid& grid, std::span<const std::int64_t> heights,
                               std::int64_t sea_level) {
    std::vector<bool> seen(grid.size(), false);
    std::vector<std::uint32_t> largest;
    std::vector<std::uint32_t> stretch;
    for (std::uint32_t start = 0; start < grid.size(); ++start) {
        if (seen[start] || heights[start] > sea_level) {
            continue;
        }
        stretch.assign(1, start);
        seen[start] = true;
        for (std::size_t next = 0; next < stretch.size(); ++next) {
            for (const std::uint32_t around : grid.neighbors(stretch[next])) {
                if (!seen[around] && heights[around] <= sea_level) {
                    seen[around] = true;
                    stretch.push_back(around);
                }
            }
        }
        if (stretch.size() > largest.size()) {
            std::swap(largest, stretch);
        }
    }
    std::ranges::sort(largest);
    return largest;
}

// Priority flood: spreads inland from the outlets, lowest first, so each cell is reached across
// the lowest rim between it and an outlet; fills `surface` and `order`.
std::vector<bool> flood(const LevelGrid& grid, std::span<const std::int64_t> heights,
                        std::int64_t sea_level, Drainage& result) {
    const std::uint32_t count = grid.size();
    Frontier frontier;
    std::vector<bool> reached(count, false);
    std::vector<bool> outlet(count, false);
    std::uint64_t reach = 0;
    const auto open = [&](std::uint32_t cell, std::int64_t surface) {
        result.surface[cell] = surface;
        reached[cell] = true;
        frontier.emplace(surface, reach++, cell);
    };
    for (const std::uint32_t cell : sea(grid, heights, sea_level)) {
        outlet[cell] = true;
        reached[cell] = true;
        result.surface[cell] = sea_level;
        result.order.push_back(cell);
    }
    // Only the coast leads inland; the open sea stays out of the queue.
    for (const std::uint32_t cell : result.order) {
        if (std::ranges::any_of(grid.neighbors(cell),
                                [&](std::uint32_t next) { return !reached[next]; })) {
            frontier.emplace(sea_level, reach++, cell);
        }
    }
    if (result.order.empty() && count > 0) {
        const auto lowest = static_cast<std::uint32_t>(
            std::ranges::distance(heights.begin(), std::ranges::min_element(heights)));
        outlet[lowest] = true;
        result.order.push_back(lowest);
        open(lowest, heights[lowest]);
    }
    while (!frontier.empty()) {
        const auto [surface, _, cell] = frontier.top();
        frontier.pop();
        if (!outlet[cell]) {
            result.order.push_back(cell);
        }
        for (const std::uint32_t next : grid.neighbors(cell)) {
            if (!reached[next]) {
                open(next, std::max(heights[next], surface + 1));
            }
        }
    }
    return outlet;
}

// The neighbour down the steepest slope of the surface. Slopes are compared as doubles: one
// correctly rounded division each, so the same on every platform.
std::uint32_t steepest(const LevelGrid& grid, std::span<const std::int64_t> surface,
                       std::uint32_t cell) {
    const auto around = grid.neighbors(cell);
    std::uint32_t best = cell;
    double best_slope = 0.0;
    for (std::size_t k = 0; k < around.size(); ++k) {
        const std::int64_t drop = surface[cell] - surface[around[k]];
        const double slope =
            static_cast<double>(drop) / static_cast<double>(grid.distance(cell, k));
        if (drop > 0 && slope > best_slope) {
            best_slope = slope;
            best = around[k];
        }
    }
    return best;
}

} // namespace

Drainage drain(const LevelGrid& grid, std::span<const std::int64_t> heights,
               std::int64_t sea_level) {
    const std::uint32_t count = grid.size();
    HOG_ASSERT(heights.size() == count, "one height per cell");
    Drainage result;
    result.surface.assign(heights.begin(), heights.end());
    result.receiver.resize(count);
    result.order.reserve(count);
    const std::vector<bool> outlet = flood(grid, heights, sea_level, result);
    HOG_ASSERT(result.order.size() == count, "every cell drains somewhere");
    for (std::uint32_t cell = 0; cell < count; ++cell) {
        result.receiver[cell] = outlet[cell] ? cell : steepest(grid, result.surface, cell);
        HOG_ASSERT(outlet[cell] || result.receiver[cell] != cell,
                   "an inland cell has a lower neighbour");
    }
    return result;
}

std::vector<std::int64_t> accumulate(const Drainage& drainage,
                                     std::span<const std::int64_t> local) {
    HOG_ASSERT(local.size() == drainage.order.size(), "one value per cell");
    std::vector<std::int64_t> total(local.begin(), local.end());
    for (auto cell = drainage.order.rbegin(); cell != drainage.order.rend(); ++cell) {
        const std::uint32_t receiver = drainage.receiver[*cell];
        if (receiver != *cell) {
            total[receiver] += total[*cell];
        }
    }
    return total;
}

} // namespace hog::planet
