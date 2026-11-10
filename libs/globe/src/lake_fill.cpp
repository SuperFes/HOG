#include "lake_fill.hpp"

#include <algorithm>
#include <array>
#include <cmath>

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

Pool::Cell Pool::at(double x, double y) const noexcept {
    const auto cell_x = static_cast<int>(std::lround(x));
    const auto cell_y = static_cast<int>(std::lround(y));
    const auto split = parts.find(key_of(std::pair{cell_x, cell_y}));
    if (split == parts.end()) {
        return at(cell_x, cell_y);
    }
    const auto part = [&](double along, int cell) {
        return std::clamp(static_cast<int>(std::floor((along - cell + 0.5) * fine)), 0, fine - 1);
    };
    return split
        ->second[(static_cast<std::size_t>(part(y, cell_y)) * static_cast<std::size_t>(fine)) +
                 static_cast<std::size_t>(part(x, cell_x))];
}

Pool pool_of(const Reached& reached) {
    Pool pool;
    pool.level = reached.level;
    pool.fine = reached.fine;
    if (reached.outlet) {
        pool.outlet = middle_of(*reached.outlet, reached.fine);
    }
    std::vector<std::pair<Spot, Pool::Cell>> marked;
    marked.reserve(reached.water.size() + reached.shore.size());
    for (const Spot spot : reached.water) {
        marked.emplace_back(spot, Pool::Cell::Water);
    }
    for (const Spot spot : reached.shore) {
        marked.emplace_back(spot, Pool::Cell::Shore);
    }
    if (marked.empty()) {
        return pool;
    }
    const auto first = cell_of(marked.front().first, reached.fine);
    int x1 = first.first;
    int y1 = first.second;
    pool.x0 = x1;
    pool.y0 = y1;
    for (const auto& [spot, kind] : marked) {
        const auto [x, y] = cell_of(spot, reached.fine);
        pool.x0 = std::min(pool.x0, x);
        pool.y0 = std::min(pool.y0, y);
        x1 = std::max(x1, x);
        y1 = std::max(y1, y);
    }
    pool.width = x1 - pool.x0 + 1;
    pool.height = y1 - pool.y0 + 1;
    pool.cells.assign(static_cast<std::size_t>(pool.width) * static_cast<std::size_t>(pool.height),
                      Pool::Cell::Dry);
    const auto fine = static_cast<std::size_t>(reached.fine);
    for (const auto& [spot, kind] : marked) {
        const auto [x, y] = cell_of(spot, reached.fine);
        const std::size_t index =
            (static_cast<std::size_t>(y - pool.y0) * static_cast<std::size_t>(pool.width)) +
            static_cast<std::size_t>(x - pool.x0);
        if (!spot.part) {
            pool.cells[index] = kind;
            continue;
        }
        // A split cell counts, as a whole, as the wettest of its parts.
        std::vector<Pool::Cell>& parts = pool.parts[key_of(std::pair{x, y})];
        parts.resize(fine * fine, Pool::Cell::Dry);
        parts[(static_cast<std::size_t>(spot.y - (y * reached.fine)) * fine) +
              static_cast<std::size_t>(spot.x - (x * reached.fine))] = kind;
        if (kind == Pool::Cell::Water || pool.cells[index] == Pool::Cell::Dry) {
            pool.cells[index] = kind;
        }
    }
    return pool;
}

} // namespace hog::globe
