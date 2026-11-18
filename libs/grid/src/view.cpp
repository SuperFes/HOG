#include <hog/grid/view.hpp>

#include <limits>

namespace hog::grid {

bool View::coarse(const Grid& grid, std::uint8_t depth, double metres) const noexcept {
    return grid.size(depth) > angle * metres;
}

std::uint8_t View::levels_at(const Grid& grid, Key root, double metres) const noexcept {
    const std::uint8_t whole = grid.levels_under(root.depth);
    for (std::uint8_t levels = 1; levels < whole; ++levels) {
        if (!coarse(grid, static_cast<std::uint8_t>(root.depth + levels), metres)) {
            return levels;
        }
    }
    return whole;
}

void View::under(const Grid& grid, Key leaf, std::uint8_t foot, const Surface& surface, DVec3 eye,
                 std::vector<Key>& pages) const {
    const double reach = surface.is_solid() || (leaf.depth < foot && !surface.crosses())
                             ? grid.finest() / angle
                             : std::numeric_limits<double>::infinity();
    std::vector<Key> pending{leaf};
    while (!pending.empty()) {
        const Key key = pending.back();
        pending.pop_back();
        // Nothing inside is nearer, so nothing inside is wanted either.
        const double metres = grid.distance(key, eye);
        if (!coarse(grid, foot, metres) || metres > reach) {
            continue;
        }
        if (key.depth == foot) {
            pages.push_back(key);
            continue;
        }
        for (std::size_t octant = 0; octant < 8; ++octant) {
            pending.push_back(key.child(octant));
        }
    }
}

} // namespace hog::grid
