#include <hog/mapgen/colour.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace hog::mapgen {

namespace {

double decode(std::uint8_t value) {
    const double encoded = static_cast<double>(value) / 255.0;
    return encoded <= 0.04045 ? encoded / 12.92 : std::pow((encoded + 0.055) / 1.055, 2.4);
}

std::uint8_t encode(double value) {
    const double linear = std::clamp(value, 0.0, 1.0);
    const double encoded =
        linear <= 0.0031308 ? linear * 12.92 : (1.055 * std::pow(linear, 1.0 / 2.4)) - 0.055;
    return static_cast<std::uint8_t>(std::lround(encoded * 255.0));
}

} // namespace

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

LinearRgb to_linear(Rgb colour) noexcept {
    return {decode(colour[0]), decode(colour[1]), decode(colour[2])};
}

Rgb to_srgb(LinearRgb colour) noexcept {
    return {encode(colour.r), encode(colour.g), encode(colour.b)};
}

} // namespace hog::mapgen
