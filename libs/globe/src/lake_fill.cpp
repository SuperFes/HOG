#include "lake_fill.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>

namespace hog::globe {

Pool::Cell Pool::at(int x, int y) const noexcept {
    const int across = x - x0;
    const int up = y - y0;
    if (across < 0 || up < 0 || across >= width || up >= height) {
        return Cell::Dry;
    }
    return cells[(static_cast<std::size_t>(up) * static_cast<std::size_t>(width)) +
                 static_cast<std::size_t>(across)];
}

Pool pool_of(const Reached& reached) {
    Pool pool;
    pool.level = reached.level;
    std::vector<bool> taken(reached.heights.size(), false);
    for (const auto& [x, y] : reached.taken) {
        taken[reached.index(x, y)] = true;
    }
    const auto under = [&](std::size_t at) {
        return taken[at] && reached.heights[at] < pool.level;
    };
    std::vector<std::pair<std::pair<int, int>, Pool::Cell>> marked;
    if (reached.taken.empty() ||
        !under(reached.index(reached.taken.front().first, reached.taken.front().second))) {
        return pool;
    }
    // Out from the first taken, through what lies under the level.
    std::vector<bool> seen(reached.heights.size(), false);
    std::vector<std::pair<int, int>> waiting{reached.taken.front()};
    seen[reached.index(waiting.front().first, waiting.front().second)] = true;
    while (!waiting.empty()) {
        const auto [x, y] = waiting.back();
        waiting.pop_back();
        marked.push_back({{x, y}, Pool::Cell::Water});
        for (const auto& [nx, ny] : std::array{std::pair{x + 1, y}, std::pair{x - 1, y},
                                               std::pair{x, y + 1}, std::pair{x, y - 1}}) {
            if (std::max(std::abs(nx), std::abs(ny)) > reached.half) {
                continue;
            }
            const std::size_t at = reached.index(nx, ny);
            if (seen[at]) {
                continue;
            }
            seen[at] = true;
            if (under(at)) {
                waiting.emplace_back(nx, ny);
            } else if (!std::isnan(reached.heights[at]) && reached.heights[at] >= pool.level) {
                marked.push_back({{nx, ny}, Pool::Cell::Shore});
            }
        }
    }
    if (marked.empty()) {
        return pool;
    }
    int x1 = marked.front().first.first;
    int y1 = marked.front().first.second;
    pool.x0 = x1;
    pool.y0 = y1;
    for (const auto& [cell, kind] : marked) {
        pool.x0 = std::min(pool.x0, cell.first);
        pool.y0 = std::min(pool.y0, cell.second);
        x1 = std::max(x1, cell.first);
        y1 = std::max(y1, cell.second);
    }
    pool.width = x1 - pool.x0 + 1;
    pool.height = y1 - pool.y0 + 1;
    pool.cells.assign(static_cast<std::size_t>(pool.width) * static_cast<std::size_t>(pool.height),
                      Pool::Cell::Dry);
    for (const auto& [cell, kind] : marked) {
        pool.cells[(static_cast<std::size_t>(cell.second - pool.y0) *
                    static_cast<std::size_t>(pool.width)) +
                   static_cast<std::size_t>(cell.first - pool.x0)] = kind;
    }
    return pool;
}

} // namespace hog::globe
