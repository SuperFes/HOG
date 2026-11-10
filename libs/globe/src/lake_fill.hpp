#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <compare>
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

// Where water may stand: a cell of a fill's grid, or, where the grid is split finer about the
// water's edge, one of the parts of one, `fine` across, counted in parts.
struct Spot {
    int x = 0;
    int y = 0;
    bool part = false;

    auto operator<=>(const Spot&) const = default;
};

// A spot as one number: cells at even counts, parts at odd, so the two never meet.
[[nodiscard]] constexpr std::int64_t key_of(Spot spot) noexcept {
    const int odd = spot.part ? 1 : 0;
    return key_of(std::pair{(2 * spot.x) + odd, (2 * spot.y) + odd});
}

// The cell a spot lies in, `fine` parts across.
[[nodiscard]] constexpr std::pair<int, int> cell_of(Spot spot, int fine) noexcept {
    if (!spot.part) {
        return {spot.x, spot.y};
    }
    const auto down = [&](int at) { return (at >= 0 ? at : at - fine + 1) / fine; };
    return {down(spot.x), down(spot.y)};
}

// A spot's middle in cells, the cells' middles at whole counts.
[[nodiscard]] constexpr std::pair<double, double> middle_of(Spot spot, int fine) noexcept {
    if (!spot.part) {
        return {spot.x, spot.y};
    }
    const auto across = [&](int at) { return ((at + 0.5) / fine) - 0.5; };
    return {across(spot.x), across(spot.y)};
}

// Water standing in a hollow, on a square grid of cells 2 × half + 1 across with the hollow's
// floor at the middle: the level it stands at, and which cells it covers, about its edge in
// parts `fine` across each cell.
struct Pool {
    enum class Cell : std::uint8_t { Dry, Water, Shore };
    double level = 0.0;
    // Where on its rim it runs out over, in cells, where it brims and finds a way out lower than
    // its floor; none where it stands short of brimming.
    std::optional<std::pair<double, double>> outlet;
    // Of the cells, the box holding all those not dry, and each cell in it.
    int x0 = 0;
    int y0 = 0;
    int width = 0;
    int height = 0;
    std::vector<Cell> cells;
    // The cells split finer, by key_of their cell, each part row by row.
    int fine = 1;
    std::unordered_map<std::int64_t, std::vector<Cell>> parts;

    // What the cell at `x`, `y` across the whole grid holds, as a whole.
    [[nodiscard]] Cell at(int x, int y) const noexcept;
    // What lies at `x`, `y`, in cells: the part there, where its cell is split.
    [[nodiscard]] Cell at(double x, double y) const noexcept;
};

// What a fill reached on a grid 2 × half + 1 across: the spots the water took in, in the order
// it did; the level and the rim it ran out over, if it did; and of the spots those it covers,
// and the shore beside them.
struct Reached {
    int half = 0;
    int fine = 1;
    std::vector<Spot> taken;
    double level = 0.0;
    std::optional<Spot> outlet;
    std::vector<Spot> water;
    std::vector<Spot> shore;

    [[nodiscard]] bool inside(Spot spot) const noexcept {
        const auto [x, y] = cell_of(spot, fine);
        return std::max(std::abs(x), std::abs(y)) <= half;
    }
    [[nodiscard]] bool edge(Spot spot) const noexcept {
        const auto [x, y] = cell_of(spot, fine);
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
// spots it reaches, so it may reach far over a wide grid. Cells may be split into parts, so it
// finds its way through rims and over shelves narrower than a cell.
template <class Ground>
class Filling {
public:
    Filling(int half, int most_half, const Ground& ground, int fine)
        : ground_{&ground}, own_{half}, fine_{std::max(1, fine)} {
        restart(half, most_half);
    }

    // Down from the middle to the bottom of the hollow it's in, the lowest way each step.
    [[nodiscard]] Spot bottom() {
        Spot at = split_.contains(key_of(std::pair{0, 0}))
                      ? Spot{.x = fine_ / 2, .y = fine_ / 2, .part = true}
                      : Spot{};
        for (bool lower = true; lower && !reached_.edge(at);) {
            lower = false;
            const Spot from = at;
            each_beside(from, [&](Spot next) {
                if (read(next) < read(at)) {
                    at = next;
                    lower = true;
                }
            });
        }
        return at;
    }

    // Lowest first from `from`, the level rising to what it takes in, until it stands at `most`,
    // runs out over a rim on down past its own reach, or reaches the grid's edge.
    void flood(Spot from, double most) {
        using Waiting = std::pair<double, Spot>;
        std::priority_queue<Waiting, std::vector<Waiting>, std::greater<>> frontier;
        // Each spot reached and the one it was reached from.
        std::unordered_map<std::int64_t, Spot> came_from;
        const auto reach = [&](Spot spot, Spot by) {
            if (came_from.try_emplace(key_of(spot), by).second) {
                frontier.push({read(spot), spot});
            }
        };
        reach(from, from);
        while (!frontier.empty()) {
            const auto [height, spot] = frontier.top();
            if (height >= most) {
                reached_.level = most;
                return;
            }
            const auto [x, y] = cell_of(spot, fine_);
            if (height < reached_.level && std::max(std::abs(x), std::abs(y)) > own_) {
                // Over a rim and on down out of its own reach: it's found its way out, over the
                // highest it came by.
                Spot back = spot;
                while (read(back) < reached_.level && back != from) {
                    back = came_from.at(key_of(back));
                }
                reached_.outlet = back;
                return;
            }
            frontier.pop();
            reached_.level = std::max(reached_.level, height);
            reached_.taken.push_back(spot);
            if (reached_.edge(spot)) {
                return;
            }
            each_beside(spot, [&](Spot next) { reach(next, spot); });
        }
    }

    // The water: out from the first taken through all that lies under the level, which the rim
    // it runs out over, the lowest way out, keeps from what lies past it. Returns the spots
    // beside it standing over the level.
    [[nodiscard]] std::vector<Spot> cover() {
        std::vector<Spot> shoreline;
        if (reached_.taken.empty() || read(reached_.taken.front()) >= reached_.level) {
            return shoreline;
        }
        seen_.clear();
        wet_.clear();
        std::vector<Spot> waiting{reached_.taken.front()};
        seen_.insert(key_of(waiting.front()));
        while (!waiting.empty()) {
            const Spot spot = waiting.back();
            waiting.pop_back();
            reached_.water.push_back(spot);
            wet_.insert(key_of(spot));
            each_beside(spot, [&](Spot next) {
                if (!reached_.inside(next) || !seen_.insert(key_of(next)).second) {
                    return;
                }
                if (read(next) < reached_.level) {
                    waiting.push_back(next);
                } else {
                    shoreline.push_back(next);
                }
            });
        }
        return shoreline;
    }

    // The shore: out from `shoreline` through spots standing over the level by less than `room`
    // a cell, never beside ground under it the water doesn't cover, where it would end in a wall.
    void shore(std::vector<Spot> shoreline, double room) {
        while (!shoreline.empty()) {
            const Spot spot = shoreline.back();
            shoreline.pop_back();
            if (bordered(spot)) {
                continue;
            }
            reached_.shore.push_back(spot);
            shore_.insert(key_of(spot));
            if (read(spot) >= reached_.level + (spot.part ? room / fine_ : room)) {
                continue;
            }
            each_beside(spot, [&](Spot next) {
                if (reached_.inside(next) && !seen_.contains(key_of(next)) &&
                    read(next) >= reached_.level) {
                    seen_.insert(key_of(next));
                    shoreline.push_back(next);
                }
            });
        }
    }

    // Splits the cells on either side of where water meets what it doesn't cover, or its shore
    // meets dry ground under its level, or kept dry only for lying beside such ground, where it
    // could stand as a wall, so the next fill finds its edge finer. A shore running out over
    // ground over the level is no wall, and is left. Returns whether any were.
    bool split_edges() {
        if (fine_ <= 1) {
            return false;
        }
        std::vector<std::pair<int, int>> splitting;
        const auto state = [&](Spot spot) {
            const std::int64_t key = key_of(spot);
            if (wet_.contains(key)) {
                return 2;
            }
            return shore_.contains(key) ? 1 : 0;
        };
        for (const auto* spots : {&reached_.water, &reached_.shore}) {
            for (const Spot spot : *spots) {
                each_beside(spot, [&](Spot next) {
                    if (!reached_.inside(next)) {
                        return;
                    }
                    const int here = state(spot);
                    const int there = state(next);
                    if (here != there &&
                        (here == 2 ||
                         (there == 0 && (read(next) < reached_.level || bordered(next))))) {
                        splitting.push_back(cell_of(spot, fine_));
                        splitting.push_back(cell_of(next, fine_));
                    }
                });
            }
        }
        bool any = false;
        for (const auto cell : splitting) {
            any = split_.insert(key_of(cell)).second || any;
        }
        return any;
    }

    // Forgets what it reached, keeping the ground read and the cells split.
    void restart(int half, int most_half) {
        reached_ = {.half = std::max(half, most_half),
                    .fine = fine_,
                    .taken = {},
                    .level = -std::numeric_limits<double>::infinity(),
                    .outlet = std::nullopt,
                    .water = {},
                    .shore = {}};
        seen_.clear();
        wet_.clear();
        shore_.clear();
    }

    [[nodiscard]] const Reached& reached() const noexcept { return reached_; }

private:
    double read(Spot spot) {
        const auto [height, added] = heights_.try_emplace(key_of(spot), 0.0);
        if (added) {
            const auto [x, y] = middle_of(spot, fine_);
            height->second = (*ground_)(x, y);
        }
        return height->second;
    }
    bool bordered(Spot spot) {
        bool found = false;
        each_beside(spot, [&](Spot next) {
            found = found || (reached_.inside(next) && !wet_.contains(key_of(next)) &&
                              read(next) < reached_.level);
        });
        return found;
    }
    [[nodiscard]] bool split(std::pair<int, int> cell) const {
        return split_.contains(key_of(cell));
    }
    // Each spot beside `spot`: the cell beside it, or where that's split, its parts along the
    // side they share.
    template <class Visit>
    void each_beside(Spot spot, Visit visit) const {
        const std::pair<int, int> cell = cell_of(spot, fine_);
        for (const auto& [x, y] : beside(spot.x, spot.y)) {
            if (spot.part) {
                const Spot next{.x = x, .y = y, .part = true};
                const std::pair<int, int> next_cell = cell_of(next, fine_);
                visit(next_cell == cell || split(next_cell)
                          ? next
                          : Spot{.x = next_cell.first, .y = next_cell.second});
                continue;
            }
            if (!split({x, y})) {
                visit(Spot{.x = x, .y = y});
                continue;
            }
            // The parts of the cell beside along its side facing this one.
            const int dx = x - cell.first;
            const int dy = y - cell.second;
            const auto facing = [&](int toward, int along) {
                if (toward == 0) {
                    return along;
                }
                return toward > 0 ? 0 : fine_ - 1;
            };
            for (int along = 0; along < fine_; ++along) {
                visit(Spot{.x = (x * fine_) + facing(dx, along),
                           .y = (y * fine_) + facing(dy, along),
                           .part = true});
            }
        }
    }

    const Ground* ground_;
    int own_;
    int fine_;
    Reached reached_;
    std::unordered_map<std::int64_t, double> heights_;
    std::unordered_set<std::int64_t> split_;
    std::unordered_set<std::int64_t> seen_;
    std::unordered_set<std::int64_t> wet_;
    std::unordered_set<std::int64_t> shore_;
};

} // namespace detail

// Water poured in at the middle of the grid, running down to the bottom of the hollow it's in,
// then rising as it fills it and whatever further hollows it runs over into, until it stands at
// `most`, or finds its way out: over a rim and on down further than `half` cells out, or to the
// grid's edge `most_half` cells out (`half`, if less). Its level is the lowest it can get out at,
// and the rim it runs on down from, its outlet. Rising, it fills as far out as the grid reaches.
// The ground at `x`, `y` in cells, read by `ground(x, y)`, is read only as the water reaches it.
//
// Water covers the cells under its level joined to the first taken under it; past the rim it runs
// out over, the cells under the level are joined only over the rim, so they stay dry. Its shore is
// the cells beside it standing over the level by less than `shore`, and those beside them so on
// out: cells whose middles stand over the level, but where some of what they hold may lie under
// it, and so stand in the water.
//
// Where `fine` is more than one, the cells about its edge are split into parts `fine` across and
// it fills again, so it finds its way through rims, and its edge, as fine as those parts, until
// its edge lies among parts or `passes` fills have run.
template <class Ground>
[[nodiscard]] Pool fill_pool(int half, const Ground& ground, double most, double shore,
                             int most_half = 0, int fine = 1, int passes = 32) {
    detail::Filling<Ground> filling{half, most_half, ground, fine};
    for (int pass = 0;; ++pass) {
        filling.flood(filling.bottom(), most);
        filling.shore(filling.cover(), shore);
        if (pass + 1 >= passes || !filling.split_edges()) {
            break;
        }
        filling.restart(half, most_half);
    }
    return pool_of(filling.reached());
}

} // namespace hog::globe
