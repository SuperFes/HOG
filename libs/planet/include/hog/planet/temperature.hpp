#pragma once

#include <hog/core/math.hpp>

#include <cstdint>

namespace hog::planet {

// Mean annual temperature: warm at the equator, cold at the poles, a few degrees either way where
// the noise has it, and colder with height.
class Temperature {
public:
    explicit Temperature(std::uint64_t seed);

    // Fixed-point °C at the sea's level under the unit direction `at`.
    [[nodiscard]] std::int64_t at_sea_level(DVec3 at) const;
    // The same at `elevation` fixed-point metres above the sea; under it, at the sea's surface.
    [[nodiscard]] std::int64_t at(DVec3 at, std::int64_t elevation) const;

private:
    std::uint64_t seed_;
};

} // namespace hog::planet
