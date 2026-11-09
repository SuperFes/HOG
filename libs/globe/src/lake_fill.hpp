#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <queue>
#include <utility>
#include <vector>

namespace hog::globe {

// Water standing in a hollow, on a square grid of cells 2 × half + 1 across with the hollow's
// floor at the middle: the level it stands at, and which cells it covers.
struct Pool {
    enum class Cell : std::uint8_t { Dry, Water, Shore };
    double level = 0.0;
    // Of the cells, the box holding all those not dry, and each cell in it.
    int x0 = 0;
    int y0 = 0;
    int width = 0;
    int height = 0;
    std::vector<Cell> cells;

    // What the cell at `x`, `y` across the whole grid holds.
    [[nodiscard]] Cell at(int x, int y) const noexcept;
};

// What a fill reached on a grid 2 × half + 1 across: each cell's ground where read (NaN where
// not), and the cells the water took in, in the order it did.
struct Reached {
    int half = 0;
    std::vector<double> heights;
    std::vector<std::pair<int, int>> taken;
    double level = 0.0;

    [[nodiscard]] std::size_t index(int x, int y) const noexcept {
        const int side = (2 * half) + 1;
        const auto across = static_cast<std::size_t>(side);
        return (static_cast<std::size_t>(y + half) * across) + static_cast<std::size_t>(x + half);
    }
};

// The pool standing where a fill reached: water over the cells under its level joined to the
// first taken under it; shore the cells beside those that stand over it, where water stands only
// on what of them lies lower. Past the rim water runs out over, the cells under the level are
// joined only over the rim, so they stay dry.
[[nodiscard]] Pool pool_of(const Reached& reached);

// Water poured in at the middle of the grid, running down to the bottom of the hollow it's in,
// then rising as it fills it and whatever further hollows it runs over into, until it stands at
// `most` or finds its way out to the grid's edge: its level is the lowest it can get out at. The
// ground of a cell, read by `ground(x, y)`, is read only as the water reaches it.
template <class Ground>
[[nodiscard]] Pool fill_pool(int half, const Ground& ground, double most) {
    const int side = (2 * half) + 1;
    const auto across = static_cast<std::size_t>(side);
    Reached reached{
        .half = half,
        .heights = std::vector<double>(across * across, std::numeric_limits<double>::quiet_NaN()),
        .taken = {},
        .level = -std::numeric_limits<double>::infinity()};
    using Waiting = std::pair<double, std::pair<int, int>>;
    std::priority_queue<Waiting, std::vector<Waiting>, std::greater<>> frontier;
    const auto read = [&](int x, int y) {
        double& height = reached.heights[reached.index(x, y)];
        if (std::isnan(height)) {
            height = ground(x, y);
        }
        return height;
    };
    std::vector<bool> queued(reached.heights.size(), false);
    const auto reach = [&](int x, int y) {
        if (!queued[reached.index(x, y)]) {
            queued[reached.index(x, y)] = true;
            frontier.push({read(x, y), {x, y}});
        }
    };
    // Downhill to the hollow's bottom first, the lowest way each step.
    std::pair<int, int> bottom{0, 0};
    for (bool lower = true; lower;) {
        lower = false;
        const auto [x, y] = bottom;
        if (x == -half || x == half || y == -half || y == half) {
            break;
        }
        for (const auto& next :
             {std::pair{x + 1, y}, std::pair{x - 1, y}, std::pair{x, y + 1}, std::pair{x, y - 1}}) {
            if (read(next.first, next.second) < read(bottom.first, bottom.second)) {
                bottom = next;
                lower = true;
            }
        }
    }
    reach(bottom.first, bottom.second);
    while (!frontier.empty()) {
        const auto [height, cell] = frontier.top();
        if (height >= most) {
            reached.level = most;
            break;
        }
        frontier.pop();
        reached.level = std::max(reached.level, height);
        reached.taken.push_back(cell);
        const auto [x, y] = cell;
        if (x == -half || x == half || y == -half || y == half) {
            break;
        }
        reach(x + 1, y);
        reach(x - 1, y);
        reach(x, y + 1);
        reach(x, y - 1);
    }
    return pool_of(reached);
}

} // namespace hog::globe
