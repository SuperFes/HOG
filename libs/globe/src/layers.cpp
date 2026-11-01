#include <hog/core/assert.hpp>
#include <hog/globe/layers.hpp>

#include <algorithm>
#include <cmath>
#include <numbers>

namespace hog::globe {

int Layers::layer_at(double radius) const noexcept {
    if (radius < floor) {
        return -1;
    }
    const double above = std::floor((radius - floor) / thickness);
    return above >= count ? count : static_cast<int>(above);
}

double Layers::volume(Cell cell, int layer) const {
    HOG_ASSERT(layer >= 0 && layer < count, "layer out of range");
    return prism_volume(cell, bottom(layer), top(layer));
}

Layers layers_about(double radius, double depth, double height, double thickness) {
    HOG_ASSERT(radius > depth && depth >= 0.0 && height > 0.0 && thickness > 0.0,
               "layers must stand outward from inside the planet");
    const auto count = std::max(1, static_cast<int>(std::lround((depth + height) / thickness)));
    const double even = (depth + height) / count;
    // Finer until a cell's mean width is under √2 layers, so it lies within √2 of one either way.
    const double side_at_root = std::sqrt(4.0 * std::numbers::pi / 6.0) * radius;
    int level = 0;
    while (level < Cell::max_level &&
           side_at_root > std::ldexp(even * std::numbers::sqrt2, level)) {
        ++level;
    }
    return {.level = level, .floor = radius - depth, .thickness = even, .count = count};
}

} // namespace hog::globe
