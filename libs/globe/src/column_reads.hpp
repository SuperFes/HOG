#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>

namespace hog::globe {

// A distance read between four columns: each one's taken by its weight, and how far the furthest
// of them strays from what's read. Where the ground's steep between them, what lies between may
// be anywhere in that.
struct ColumnRead {
    double distance = 0.0;
    double spread = 0.0;
};

[[nodiscard]] inline ColumnRead read_between(const std::array<double, 4>& distances,
                                             const std::array<double, 4>& weights) noexcept {
    ColumnRead read;
    for (std::size_t k = 0; k < 4; ++k) {
        read.distance += weights.at(k) * distances.at(k);
    }
    for (const double one : distances) {
        read.spread = std::max(read.spread, std::abs(one - read.distance));
    }
    return read;
}

// Whether a node `size` across, read so, lies further from the surface than `margin` of its sides
// and the columns' spread together.
[[nodiscard]] inline bool clear_of_surface(const ColumnRead& read, double size,
                                           double margin) noexcept {
    return std::abs(read.distance) > (margin * size) + read.spread;
}

} // namespace hog::globe
