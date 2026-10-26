#pragma once

#include <hog/core/math.hpp>
#include <hog/planet/cell_id.hpp>
#include <hog/planet/continents.hpp>

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace hog::planet {

// Plumes rising from deep below, fixed while the plates move over them: each swells the ocean floor
// above it and builds a chain of shield volcanoes, the youngest over the plume and the older ones
// carried off with the plate, sinking as they go until only seamounts are left.
class Hotspots {
public:
    static constexpr int count = 3;
    static constexpr int chain_length = 8;
    // Metres above the sea the youngest shield rises, and the most the swell raises the floor.
    static constexpr double young_summit = 1'200.0;
    static constexpr double swell_height = 1'500.0;

    struct Shield {
        DVec3 center;  // unit direction
        double summit; // metres above sea level, negative once sunk
    };
    struct Hotspot {
        DVec3 center;  // unit direction
        DVec3 heading; // unit tangent the plate carries the chain along
    };

    Hotspots(std::uint64_t seed, double planet_radius, const Continents& continents);

    [[nodiscard]] std::span<const Hotspot> hotspots() const noexcept { return hotspots_; }
    // Each hotspot's chain in turn, youngest first.
    [[nodiscard]] std::span<const Shield> shields() const noexcept { return shields_; }

    // Fixed-point metres, with `Bits` fractional bits, the swells raise the ocean floor at a cell.
    template <int Bits = fixed_fraction_bits>
    [[nodiscard]] std::int64_t swell(CellId cell) const;
    // Fixed-point metres above sea level, with `Bits` fractional bits, of the highest shield's
    // flank at a cell, where one reaches it.
    template <int Bits = fixed_fraction_bits>
    [[nodiscard]] std::optional<std::int64_t> surface(CellId cell) const;

private:
    double planet_radius_;
    std::uint64_t lobe_seed_;
    std::uint64_t rough_seed_;
    std::vector<Hotspot> hotspots_;
    std::vector<Shield> shields_;
};

extern template std::int64_t Hotspots::swell<fixed_fraction_bits>(CellId cell) const;
extern template std::int64_t Hotspots::swell<fine_fraction_bits>(CellId cell) const;
extern template std::optional<std::int64_t>
Hotspots::surface<fixed_fraction_bits>(CellId cell) const;
extern template std::optional<std::int64_t>
Hotspots::surface<fine_fraction_bits>(CellId cell) const;

} // namespace hog::planet
