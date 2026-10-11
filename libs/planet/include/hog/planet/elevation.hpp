#pragma once

#include <hog/planet/cell_id.hpp>
#include <hog/planet/continents.hpp>

#include <cstdint>

namespace hog::planet {

// Terrain height: continents give the coastline, lowlands and ocean floor; eroded ridges raise
// mountain ranges inside belts away from the coast.
class Elevation {
public:
    // Metres.
    static constexpr std::int64_t ocean_depth = 4'000;
    static constexpr std::int64_t lowland_height = 300;
    static constexpr std::int64_t mountain_height = 4'000;

    Elevation(std::uint64_t seed, double ocean_fraction);

    [[nodiscard]] const Continents& continents() const noexcept { return continents_; }

    // Fixed-point metres above sea level: positive on land, negative under the ocean.
    [[nodiscard]] std::int64_t elevation(CellId cell) const;

    // How strongly mountains rise here, in fixed point from 0 to 1.
    [[nodiscard]] std::int64_t mountain_strength(CellId cell) const;

private:
    [[nodiscard]] std::int64_t mountain_strength(CellId cell, std::int64_t height) const;

    Continents continents_;
    std::uint64_t belt_seed_;
    std::uint64_t ridge_seed_;
};

} // namespace hog::planet
