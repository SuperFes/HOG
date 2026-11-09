#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <limits>
#include <optional>
#include <queue>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace hog::globe {

// A cell as one number, for the sets a fill keeps.
[[nodiscard]] constexpr std::int64_t key_of(std::pair<int, int> cell) noexcept {
    return (static_cast<std::int64_t>(cell.first) << 32) ^
           static_cast<std::int64_t>(static_cast<std::uint32_t>(cell.second));
}

// Water standing in a hollow, on a square grid of cells 2 × half + 1 across with the hollow's
// floor at the middle: the level it stands at, and which cells it covers.
struct Pool {
    enum class Cell : std::uint8_t { Dry, Water, Shore };
    double level = 0.0;
    // The cell on its rim it runs out over, where it brims and finds a way out lower than its
    // floor; none where it stands short of brimming.
    std::optional<std::pair<int, int>> outlet;
    // Of the cells, the box holding all those not dry, and each cell in it.
    int x0 = 0;
    int y0 = 0;
    int width = 0;
    int height = 0;
    std::vector<Cell> cells;

    // What the cell at `x`, `y` across the whole grid holds.
    [[nodiscard]] Cell at(int x, int y) const noexcept;
};

// What a fill reached on a grid 2 × half + 1 across: the cells the water took in, in the order
// it did; the level and the rim it ran out over, if it did; and of the cells those it covers, and
// the shore beside them.
struct Reached {
    int half = 0;
    std::vector<std::pair<int, int>> taken;
    double level = 0.0;
    std::optional<std::pair<int, int>> outlet;
    std::vector<std::pair<int, int>> water;
    std::vector<std::pair<int, int>> shore;

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

// A fill over ground read by `Ground`, as fill_pool does it, a step at a time. It keeps only the
// cells it reaches, so it may reach far over a wide grid.
template <class Ground>
class Filling {
public:
    Filling(int half, int most_half, const Ground& ground) : ground_{&ground}, own_{half} {
        reached_ = {.half = std::max(half, most_half),
                    .taken = {},
                    .level = -std::numeric_limits<double>::infinity(),
                    .outlet = std::nullopt,
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

    // Lowest first from `from`, the level rising to what it takes in, until it stands at `most`,
    // runs out over a rim on down past its own reach, or reaches the grid's edge.
    void flood(std::pair<int, int> from, double most) {
        using Waiting = std::pair<double, std::pair<int, int>>;
        std::priority_queue<Waiting, std::vector<Waiting>, std::greater<>> frontier;
        // Each cell reached and the one it was reached from.
        std::unordered_map<std::int64_t, std::pair<int, int>> came_from;
        const auto reach = [&](std::pair<int, int> cell, std::pair<int, int> by) {
            if (came_from.try_emplace(key_of(cell), by).second) {
                frontier.push({read(cell), cell});
            }
        };
        reach(from, from);
        while (!frontier.empty()) {
            const auto [height, cell] = frontier.top();
            if (height >= most) {
                reached_.level = most;
                return;
            }
            if (height < reached_.level &&
                std::max(std::abs(cell.first), std::abs(cell.second)) > own_) {
                // Over a rim and on down out of its own reach: it's found its way out, over the
                // highest it came by.
                std::pair<int, int> back = cell;
                while (read(back) < reached_.level && back != from) {
                    back = came_from.at(key_of(back));
                }
                reached_.outlet = back;
                return;
            }
            frontier.pop();
            reached_.level = std::max(reached_.level, height);
            reached_.taken.push_back(cell);
            if (reached_.edge(cell.first, cell.second)) {
                return;
            }
            for (const auto& next : beside(cell.first, cell.second)) {
                reach(next, cell);
            }
        }
    }

    // The water: out from the first taken through all that lies under the level, which the rim
    // it runs out over, the lowest way out, keeps from what lies past it. Returns the cells
    // beside it standing over the level.
    [[nodiscard]] std::vector<std::pair<int, int>> cover() {
        std::vector<std::pair<int, int>> shoreline;
        if (reached_.taken.empty() || read(reached_.taken.front()) >= reached_.level) {
            return shoreline;
        }
        seen_.clear();
        wet_.clear();
        std::vector<std::pair<int, int>> waiting{reached_.taken.front()};
        seen_.insert(key_of(waiting.front()));
        while (!waiting.empty()) {
            const std::pair<int, int> cell = waiting.back();
            waiting.pop_back();
            reached_.water.push_back(cell);
            wet_.insert(key_of(cell));
            for (const auto& next : beside(cell.first, cell.second)) {
                if (!reached_.inside(next.first, next.second) ||
                    !seen_.insert(key_of(next)).second) {
                    continue;
                }
                if (read(next) < reached_.level) {
                    waiting.push_back(next);
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
                if (reached_.inside(next.first, next.second) && !seen_.contains(key_of(next)) &&
                    read(next) >= reached_.level) {
                    seen_.insert(key_of(next));
                    shoreline.push_back(next);
                }
            }
        }
    }

    [[nodiscard]] const Reached& reached() const noexcept { return reached_; }

private:
    double read(std::pair<int, int> cell) {
        const auto [height, added] = heights_.try_emplace(key_of(cell), 0.0);
        if (added) {
            height->second = (*ground_)(cell.first, cell.second);
        }
        return height->second;
    }
    bool bordered(std::pair<int, int> cell) {
        return std::ranges::any_of(beside(cell.first, cell.second), [&](const auto& next) {
            return reached_.inside(next.first, next.second) && !wet_.contains(key_of(next)) &&
                   read(next) < reached_.level;
        });
    }

    const Ground* ground_;
    int own_;
    Reached reached_;
    std::unordered_map<std::int64_t, double> heights_;
    std::unordered_set<std::int64_t> seen_;
    std::unordered_set<std::int64_t> wet_;
};

} // namespace detail

// Water poured in at the middle of the grid, running down to the bottom of the hollow it's in,
// then rising as it fills it and whatever further hollows it runs over into, until it stands at
// `most`, or finds its way out: over a rim and on down further than `half` cells out, or to the
// grid's edge `most_half` cells out (`half`, if less). Its level is the lowest it can get out at,
// and the rim it runs on down from, its outlet. Rising, it fills as far out as the grid reaches.
// The ground of a cell, read by `ground(x, y)`, is read only as the water reaches it.
//
// Water covers the cells under its level joined to the first taken under it; past the rim it runs
// out over, the cells under the level are joined only over the rim, so they stay dry. Its shore is
// the cells beside it standing over the level by less than `shore`, and those beside them so on
// out: cells whose middles stand over the level, but where some of what they hold may lie under
// it, and so stand in the water.
template <class Ground>
[[nodiscard]] Pool fill_pool(int half, const Ground& ground, double most, double shore,
                             int most_half = 0) {
    detail::Filling<Ground> filling{half, most_half, ground};
    filling.flood(filling.bottom(), most);
    filling.shore(filling.cover(), shore);
    return pool_of(filling.reached());
}

} // namespace hog::globe
