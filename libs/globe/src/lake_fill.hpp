#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
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
// not); the cells the water took in, in the order it did; and of them those it covers, and the
// shore beside them.
struct Reached {
    int half = 0;
    std::vector<double> heights;
    std::vector<std::pair<int, int>> taken;
    double level = 0.0;
    std::vector<std::pair<int, int>> water;
    std::vector<std::pair<int, int>> shore;

    [[nodiscard]] std::size_t index(int x, int y) const noexcept {
        const int side = (2 * half) + 1;
        const auto across = static_cast<std::size_t>(side);
        return (static_cast<std::size_t>(y + half) * across) + static_cast<std::size_t>(x + half);
    }
    [[nodiscard]] bool inside(int x, int y) const noexcept {
        return std::max(std::abs(x), std::abs(y)) <= half;
    }
    [[nodiscard]] bool edge(int x, int y) const noexcept {
        return std::max(std::abs(x), std::abs(y)) == half;
    }
};

// The pool standing where a fill reached, its water and shore laid out.
[[nodiscard]] Pool pool_of(const Reached& reached);

// The cells beside `x`, `y`.
[[nodiscard]] inline std::array<std::pair<int, int>, 4> beside(int x, int y) noexcept {
    return {std::pair{x + 1, y}, std::pair{x - 1, y}, std::pair{x, y + 1}, std::pair{x, y - 1}};
}

namespace detail {

// A fill over ground read by `Ground`, as fill_pool does it, a step at a time.
template <class Ground>
class Filling {
public:
    Filling(int half, const Ground& ground) : ground_{&ground} {
        const int side = (2 * half) + 1;
        const auto across = static_cast<std::size_t>(side);
        reached_ = {.half = half,
                    .heights = std::vector<double>(across * across,
                                                   std::numeric_limits<double>::quiet_NaN()),
                    .taken = {},
                    .level = -std::numeric_limits<double>::infinity(),
                    .water = {},
                    .shore = {}};
    }

    // Down from the middle to the bottom of the hollow it's in, the lowest way each step.
    [[nodiscard]] std::pair<int, int> bottom() {
        std::pair<int, int> at{0, 0};
        for (bool lower = true; lower && !reached_.edge(at.first, at.second);) {
            lower = false;
            for (const auto& next : beside(at.first, at.second)) {
                if (read(next) < read(at)) {
                    at = next;
                    lower = true;
                }
            }
        }
        return at;
    }

    // Lowest first from `from`, the level rising to what it takes in, until it stands at `most`
    // or reaches the grid's edge.
    void flood(std::pair<int, int> from, double most) {
        using Waiting = std::pair<double, std::pair<int, int>>;
        std::priority_queue<Waiting, std::vector<Waiting>, std::greater<>> frontier;
        std::vector<std::uint8_t> queued(reached_.heights.size(), 0);
        const auto reach = [&](std::pair<int, int> cell) {
            if (std::exchange(queued[index(cell)], 1) == 0) {
                frontier.push({read(cell), cell});
            }
        };
        reach(from);
        while (!frontier.empty()) {
            const auto [height, cell] = frontier.top();
            if (height >= most) {
                reached_.level = most;
                return;
            }
            frontier.pop();
            reached_.level = std::max(reached_.level, height);
            reached_.taken.push_back(cell);
            if (reached_.edge(cell.first, cell.second)) {
                return;
            }
            for (const auto& next : beside(cell.first, cell.second)) {
                reach(next);
            }
        }
    }

    // The water: out from the first taken through what lies under the level. Returns the cells
    // beside it standing over the level.
    [[nodiscard]] std::vector<std::pair<int, int>> cover() {
        std::vector<std::pair<int, int>> shoreline;
        if (reached_.taken.empty() || read(reached_.taken.front()) >= reached_.level) {
            return shoreline;
        }
        std::vector<std::uint8_t> taken(reached_.heights.size(), 0);
        for (const auto& cell : reached_.taken) {
            taken[index(cell)] = 1;
        }
        seen_.assign(reached_.heights.size(), 0);
        wet_.assign(reached_.heights.size(), 0);
        std::vector<std::pair<int, int>> waiting{reached_.taken.front()};
        seen_[index(waiting.front())] = 1;
        while (!waiting.empty()) {
            const std::pair<int, int> cell = waiting.back();
            waiting.pop_back();
            reached_.water.push_back(cell);
            wet_[index(cell)] = 1;
            for (const auto& next : beside(cell.first, cell.second)) {
                if (!reached_.inside(next.first, next.second) ||
                    std::exchange(seen_[index(next)], 1) != 0) {
                    continue;
                }
                if (read(next) < reached_.level) {
                    if (taken[index(next)] != 0) {
                        waiting.push_back(next);
                    }
                } else {
                    shoreline.push_back(next);
                }
            }
        }
        return shoreline;
    }

    // The shore: out from `shoreline` through cells standing over the level by less than `room`,
    // never beside ground under it the water doesn't cover, where it would end in a wall.
    void shore(std::vector<std::pair<int, int>> shoreline, double room) {
        while (!shoreline.empty()) {
            const std::pair<int, int> cell = shoreline.back();
            shoreline.pop_back();
            if (bordered(cell)) {
                continue;
            }
            reached_.shore.push_back(cell);
            if (read(cell) >= reached_.level + room) {
                continue;
            }
            for (const auto& next : beside(cell.first, cell.second)) {
                if (reached_.inside(next.first, next.second) && seen_[index(next)] == 0 &&
                    read(next) >= reached_.level) {
                    seen_[index(next)] = 1;
                    shoreline.push_back(next);
                }
            }
        }
    }

    [[nodiscard]] const Reached& reached() const noexcept { return reached_; }

private:
    [[nodiscard]] std::size_t index(std::pair<int, int> cell) const noexcept {
        return reached_.index(cell.first, cell.second);
    }
    double read(std::pair<int, int> cell) {
        double& height = reached_.heights[index(cell)];
        if (std::isnan(height)) {
            height = (*ground_)(cell.first, cell.second);
        }
        return height;
    }
    bool bordered(std::pair<int, int> cell) {
        return std::ranges::any_of(beside(cell.first, cell.second), [&](const auto& next) {
            return reached_.inside(next.first, next.second) && wet_[index(next)] == 0 &&
                   read(next) < reached_.level;
        });
    }

    const Ground* ground_;
    Reached reached_;
    std::vector<std::uint8_t> seen_;
    std::vector<std::uint8_t> wet_;
};

} // namespace detail

// Water poured in at the middle of the grid, running down to the bottom of the hollow it's in,
// then rising as it fills it and whatever further hollows it runs over into, until it stands at
// `most` or finds its way out to the grid's edge: its level is the lowest it can get out at. The
// ground of a cell, read by `ground(x, y)`, is read only as the water reaches it.
//
// Water covers the cells under its level joined to the first taken under it; past the rim it runs
// out over, the cells under the level are joined only over the rim, so they stay dry. Its shore is
// the cells beside it standing over the level by less than `shore`, and those beside them so on
// out: cells whose middles stand over the level, but where some of what they hold may lie under
// it, and so stand in the water.
template <class Ground>
[[nodiscard]] Pool fill_pool(int half, const Ground& ground, double most, double shore) {
    detail::Filling<Ground> filling{half, ground};
    filling.flood(filling.bottom(), most);
    filling.shore(filling.cover(), shore);
    return pool_of(filling.reached());
}

} // namespace hog::globe
