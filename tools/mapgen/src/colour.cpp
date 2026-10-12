#include <hog/mapgen/colour.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace hog::mapgen {

Rgb ramp(std::span<const ColourStop> stops, double value) noexcept {
    if (value <= stops.front().at) {
        return stops.front().colour;
    }
    for (std::size_t index = 1; index < stops.size(); ++index) {
        const ColourStop& high = stops[index];
        if (value <= high.at) {
            const ColourStop& low = stops[index - 1];
            return mix(low.colour, high.colour, (value - low.at) / (high.at - low.at));
        }
    }
    return stops.back().colour;
}

Rgb mix(Rgb from, Rgb to, double share) noexcept {
    const double t = std::clamp(share, 0.0, 1.0);
    Rgb out{};
    for (std::size_t channel = 0; channel < out.size(); ++channel) {
        out.at(channel) = static_cast<std::uint8_t>(std::lround(
            static_cast<double>(from.at(channel)) +
            ((static_cast<double>(to.at(channel)) - static_cast<double>(from.at(channel))) * t)));
    }
    return out;
}

} // namespace hog::mapgen
