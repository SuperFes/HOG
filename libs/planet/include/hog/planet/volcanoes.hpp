#pragma once

#include <hog/core/math.hpp>
#include <hog/planet/cell_id.hpp>
#include <hog/planet/continents.hpp>
#include <hog/planet/level_grid.hpp>

#include <cstdint>
#include <span>
#include <vector>

namespace hog::planet {

// Volcanoes rising in the arcs above plates diving: cones with a crater at the summit, standing on
// whatever ground is under them, so out at sea the tallest break the surface as islands.
class Volcanoes {
public:
    // Metres a cone rises at most to the peak its crater cuts, and its flanks' rise over their run
    // on average, steeper near the summit and gentler at the foot.
    static constexpr double max_height = 2'500.0;
    static constexpr double flank_slope = 0.45;
    // Lava flows and gullies push the foot out, or pull it in, up to this much of the way.
    static constexpr double lobes = 0.25;

    struct Volcano {
        DVec3 center;  // unit direction
        double height; // metres from the foot to the peak the crater cuts
        double radius; // metres from the centre to the foot, before lobes move it
    };

    Volcanoes(std::uint64_t seed, double planet_radius, const Continents& continents);

    [[nodiscard]] std::span<const Volcano> volcanoes() const noexcept { return volcanoes_; }

    // Fixed-point metres, with `Bits` fractional bits, that the volcanoes raise the ground at a
    // cell.
    template <int Bits = fixed_fraction_bits>
    [[nodiscard]] std::int64_t lift(CellId cell) const;

private:
    static constexpr std::uint32_t none = ~std::uint32_t{0};

    double planet_radius_;
    std::uint64_t lobe_seed_;
    LevelGrid grid_;
    // Per grid cell, the volcano standing in it, or none.
    std::vector<std::uint32_t> standing_;
    std::vector<Volcano> volcanoes_;
};

extern template std::int64_t Volcanoes::lift<fixed_fraction_bits>(CellId cell) const;
extern template std::int64_t Volcanoes::lift<fine_fraction_bits>(CellId cell) const;

} // namespace hog::planet
