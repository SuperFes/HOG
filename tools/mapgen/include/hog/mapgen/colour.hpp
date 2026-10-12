#pragma once

#include <array>
#include <cstdint>
#include <span>

namespace hog::mapgen {

using Rgb = std::array<std::uint8_t, 3>;

struct ColourStop {
    double at; // the value where the colour is exact
    Rgb colour;
};

// The colour between the stops around `value`, holding the end colours beyond them. Precondition:
// the stops ascend.
[[nodiscard]] Rgb ramp(std::span<const ColourStop> stops, double value) noexcept;

// From one colour towards another by `share`, from 0 to 1.
[[nodiscard]] Rgb mix(Rgb from, Rgb to, double share) noexcept;

} // namespace hog::mapgen
