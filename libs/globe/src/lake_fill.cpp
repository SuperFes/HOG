#include "lake_fill.hpp"

#include <algorithm>
#include <array>

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
    std::vector<std::pair<std::pair<int, int>, Pool::Cell>> marked;
    marked.reserve(reached.water.size() + reached.shore.size());
    for (const auto& cell : reached.water) {
        marked.emplace_back(cell, Pool::Cell::Water);
    }
    for (const auto& cell : reached.shore) {
        marked.emplace_back(cell, Pool::Cell::Shore);
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
