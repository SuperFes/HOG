#include <hog/core/assert.hpp>
#include <hog/grid/grid.hpp>

#include <algorithm>
#include <cmath>

namespace hog::grid {

Grid::Grid(double reach, double finest) : finest_(finest) {
    HOG_ASSERT(finest > 0.0 && reach > 0.0, "a grid needs a size and something to hold");
    while (std::ldexp(finest, depth_) < 2.0 * reach) {
        ++depth_;
        HOG_ASSERT(depth_ <= max_depth, "the root must lie within the deepest keys");
    }
}

double Grid::size(std::uint8_t at) const noexcept {
    return std::ldexp(finest_, depth_ - at);
}

DVec3 Grid::low(Key key) const noexcept {
    const double half = size(0) / 2.0;
    const double step = size(key.depth);
    return DVec3{(key.x * step) - half, (key.y * step) - half, (key.z * step) - half};
}

DVec3 Grid::centre(Key key) const noexcept {
    return low(key) + DVec3{size(key.depth) / 2.0};
}

double Grid::distance(Key key, DVec3 point) const noexcept {
    const DVec3 near = low(key);
    return glm::length(point - glm::clamp(point, near, near + DVec3{size(key.depth)}));
}

std::optional<Key> Grid::key_at(DVec3 point, std::uint8_t at) const noexcept {
    const double half = size(0) / 2.0;
    const double step = size(at);
    const std::uint32_t last = (std::uint32_t{1} << at) - 1U;
    const auto along = [&](double coordinate) -> std::optional<std::uint32_t> {
        const double from_low = coordinate + half;
        if (!std::isfinite(from_low) || from_low < 0.0 || from_low >= 2.0 * half) {
            return std::nullopt;
        }
        // Rounding can land a point just inside the high side one node past the last.
        return std::min(static_cast<std::uint32_t>(std::floor(from_low / step)), last);
    };
    const auto x = along(point.x);
    const auto y = along(point.y);
    const auto z = along(point.z);
    if (!x || !y || !z) {
        return std::nullopt;
    }
    return Key{.depth = at, .x = *x, .y = *y, .z = *z};
}

Key Grid::page_of(Key key) const noexcept {
    if (key.depth == 0) {
        return key;
    }
    const int top = depth_ % page_levels;
    const int above = key.depth - 1;
    const int cut = above < top ? 0 : top + (page_levels * ((above - top) / page_levels));
    return key.ancestor(static_cast<std::uint8_t>(cut));
}

std::uint8_t Grid::levels_under(std::uint8_t at) const noexcept {
    const int top = depth_ % page_levels;
    const int span = at == 0 && top > 0 ? top : page_levels;
    return static_cast<std::uint8_t>(std::min(span, depth_ - at));
}

} // namespace hog::grid
