#pragma once

#include <hog/core/math.hpp>
#include <hog/planet/cell_id.hpp>
#include <hog/planet/continents.hpp>

#include <cstdint>
#include <span>
#include <vector>

namespace hog::planet {

// The starting islands: a chain raised from open ocean in a temperate band, each island within
// sailing distance of another, each built up around a volcano, the first the youngest and tallest.
class Archipelago {
public:
    static constexpr int min_islands = 5;
    static constexpr int max_islands = 8;
    // Square metres of undisturbed coastline; the coastline noise moves real areas either side.
    static constexpr double min_target_area = 15e6;
    static constexpr double max_target_area = 45e6;
    // Longest over shortest width of an undisturbed coast, which is an ellipse.
    static constexpr double max_elongation = 2.5;
    // Metres of water between an island and its closest neighbour when placed. The coastline
    // noise can pull two facing coasts back by nearly 5 km between them, and the islands must stay
    // within 8 km, a short crossing.
    static constexpr double min_gap = 1'500.0;
    static constexpr double max_gap = 3'000.0;
    // Metres from the centre that every island stays within.
    static constexpr double reach = 35'000.0;
    // Metres around the centre that must be open ocean.
    static constexpr double clearance = 45'000.0;
    // Continent height lost per metre down an island's underwater flank.
    static constexpr double skirt_slope = 1e-5;

    struct Island {
        DVec3 center;      // unit direction
        double radius;     // metres to a circular coast of the same area
        DVec3 axis;        // unit tangent along the longest width
        double elongation; // longest over shortest width, at least 1
        std::int64_t peak; // fixed-point continent height at the centre
        double summit;     // metres the volcano rises above the lowland at the centre
    };

    Archipelago(std::uint64_t seed, double planet_radius, const Continents& continents);

    [[nodiscard]] DVec3 center() const noexcept { return center_; }
    [[nodiscard]] std::span<const Island> islands() const noexcept { return islands_; }

    // The higher of `height` and the islands' continent height at the cell, both in fixed point
    // with `Bits` fractional bits.
    template <int Bits = fixed_fraction_bits>
    [[nodiscard]] std::int64_t raise(CellId cell, std::int64_t height) const;
    // Fixed-point metres, with `Bits` fractional bits, the islands' volcanoes rise above their
    // lowland at a cell, falling to nothing at the coast.
    template <int Bits = fixed_fraction_bits>
    [[nodiscard]] std::int64_t volcano(CellId cell) const;

private:
    // How far the coast moves out at a unit direction, as a fraction of an island's radius.
    [[nodiscard]] double coast_moved(DVec3 position) const;
    // Metres from an island's heart to an offset from its centre, as an undisturbed island's
    // would be, reaching its radius at the coast however far the coast moves.
    [[nodiscard]] static double from_heart(const Island& island, DVec3 between, double wobble);

    double planet_radius_;
    std::uint64_t noise_seed_;
    double skirt_reach_; // metres
    DVec3 center_{};
    std::vector<Island> islands_;
};

extern template std::int64_t Archipelago::raise<fixed_fraction_bits>(CellId cell,
                                                                     std::int64_t height) const;
extern template std::int64_t Archipelago::raise<fine_fraction_bits>(CellId cell,
                                                                    std::int64_t height) const;
extern template std::int64_t Archipelago::volcano<fixed_fraction_bits>(CellId cell) const;
extern template std::int64_t Archipelago::volcano<fine_fraction_bits>(CellId cell) const;

} // namespace hog::planet
