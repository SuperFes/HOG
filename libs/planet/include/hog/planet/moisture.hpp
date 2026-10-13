#pragma once

#include <hog/planet/cell_id.hpp>

#include <cstdint>

namespace hog::planet {

// Wetness from Earth-like circulation bands, drying out deep inland. It depends only on the seed
// and the continents, so the terrain can be shaped by it before the climate exists.
class Moisture {
public:
    explicit Moisture(std::uint64_t seed);

    // Fixed point from 0 (arid) to 1 (saturated), given the fixed-point continent height.
    [[nodiscard]] std::int64_t at(CellId cell, std::int64_t continent_height) const;

private:
    std::uint64_t seed_;
};

} // namespace hog::planet
