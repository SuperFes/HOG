#include <hog/core/assert.hpp>
#include <hog/globe/layers.hpp>

#include <algorithm>
#include <cmath>

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
    return {.floor = radius - depth, .thickness = (depth + height) / count, .count = count};
}

} // namespace hog::globe
