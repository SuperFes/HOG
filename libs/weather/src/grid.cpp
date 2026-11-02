#include <hog/weather/grid.hpp>

#include <cmath>

namespace hog::weather {

std::optional<int> Grid::layer_at(double radius) const noexcept {
    const double layer = std::floor((radius - bottom) / thickness);
    if (!(layer >= 0.0) || layer >= static_cast<double>(layers)) {
        return std::nullopt;
    }
    return static_cast<int>(layer);
}

} // namespace hog::weather
