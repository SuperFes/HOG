#pragma once

#include <hog/core/math.hpp>
#include <hog/planet/cell_id.hpp>
#include <hog/planet/continents.hpp>

#include <cstdint>
#include <span>
#include <vector>

namespace hog::planet {

// The starting islands: a chain raised from open ocean in a temperate band, each island within
// sailing distance of another.
class Archipelago {
public:
    static constexpr int min_islands = 5;
    static constexpr int max_islands = 8;
    // Square metres of undisturbed coastline; the coastline noise moves real areas either side.
    static constexpr double min_target_area = 15e6;
    static constexpr double max_target_area = 45e6;
    // Metres of water between an island and its closest neighbour when placed.
    static constexpr double min_gap = 1'500.0;
    static constexpr double max_gap = 6'000.0;
    // Metres from the centre that every island stays within.
    static constexpr double reach = 25'000.0;
    // Metres around the centre that must be open ocean.
    static constexpr double clearance = 35'000.0;

    struct Island {
        DVec3 center;      // unit direction
        double radius;     // metres to the undisturbed coast
        std::int64_t peak; // fixed-point continent height at the centre
    };

    Archipelago(std::uint64_t seed, double planet_radius, const Continents& continents);

    [[nodiscard]] DVec3 center() const noexcept { return center_; }
    [[nodiscard]] std::span<const Island> islands() const noexcept { return islands_; }

    // The higher of `height` and the islands' fixed-point continent height at the cell.
    [[nodiscard]] std::int64_t raise(CellId cell, std::int64_t height) const;

private:
    double planet_radius_;
    std::uint64_t noise_seed_;
    double skirt_reach_; // metres
    DVec3 center_{};
    std::vector<Island> islands_;
};

} // namespace hog::planet
