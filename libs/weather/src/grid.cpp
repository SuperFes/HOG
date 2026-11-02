#include <hog/weather/grid.hpp>

#include <algorithm>
#include <cmath>

namespace hog::weather {

double Grid::floor_of(int layer) const noexcept {
    const auto n = static_cast<double>(layer);
    if (ratio == 1.0) {
        return bottom + (n * thickness);
    }
    return bottom + (thickness * (std::pow(ratio, n) - 1.0) / (ratio - 1.0));
}

std::optional<int> Grid::layer_at(double radius) const noexcept {
    const double up = radius - bottom;
    if (!(up >= 0.0) || radius >= top()) {
        return std::nullopt;
    }
    double guess = up / thickness;
    if (ratio != 1.0) {
        guess = std::log1p(up * (ratio - 1.0) / thickness) / std::log(ratio);
    }
    int layer = std::min(static_cast<int>(guess), layers - 1);
    // The closed form can land a layer off at an edge.
    while (layer > 0 && radius < floor_of(layer)) {
        --layer;
    }
    while (layer + 1 < layers && radius >= floor_of(layer + 1)) {
        ++layer;
    }
    return layer;
}

} // namespace hog::weather
