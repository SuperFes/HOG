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

int level_about(double radius, double thickness) noexcept {
    const double side_at_root = std::sqrt(4.0 * std::numbers::pi / 6.0) * radius;
    int level = 0;
    while (level < Cell::max_level &&
           side_at_root > std::ldexp(thickness * std::numbers::sqrt2, level)) {
        ++level;
    }
    return level;
}

Layers layers_about(double radius, double depth, double height, double thickness) {
    HOG_ASSERT(radius > depth && depth >= 0.0 && height > 0.0 && thickness > 0.0,
               "layers must stand outward from inside the planet");
    // Whole layers out from the reference level each way, so it lies on a layer's floor.
    const auto below = static_cast<int>(std::ceil(depth / thickness));
    const auto above = static_cast<int>(std::ceil(height / thickness));
    return {.level = level_about(radius, thickness),
            .floor = radius - (static_cast<double>(below) * thickness),
            .thickness = thickness,
            .count = std::max(1, below + above)};
}

} // namespace hog::globe
