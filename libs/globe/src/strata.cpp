#include "terms.hpp"

#include <hog/core/hash.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace hog::globe::terms {
namespace {

// Platforms of sedimentary rock lie up to a kilometre and a half deep over continental basement
// where basins sag, in basins some 400 km across on Earth; the ocean floor gathers a few hundred
// metres. Mountains strip it, and high ground loses half its height of it.
constexpr double platform_cover = 1'500.0;
constexpr double basin_wavelength = 4.0e5;
constexpr double ocean_cover = 400.0;

// Beds lie nearly flat under plains, rippling tens of metres; where mountains were built they
// fold hundreds.
constexpr double fold_wavelength = 3'000.0;
constexpr double plain_fold = 20.0;
constexpr double belt_fold = 600.0;

// Soil a metre and a half deep where water weathers rock, none on slopes steeper than it holds
// to; alluvium and colluvium a few metres under gentle ground.
constexpr double soil_depth = 1.5;
constexpr double sediment_depth = 4.0;

// Impacts garden the ground into regolith a few metres a billion years where nothing weathers
// it; under air, slower, as dust settles and wind sweeps.
constexpr double regolith_per_gyr = 2.5;
constexpr double regolith_per_gyr_under_air = 1.0;

// Snow outlasts the year where its mean is a few degrees below freezing.
constexpr double snow_below = 268.0;
constexpr double snow_depth = 2.0;

// Beaches: sand within a few metres of the sea's level on gentle shores.
constexpr double beach_below = 10.0;
constexpr double beach_above = 3.0;
constexpr double beach_depth = 3.0;

double unit(std::uint64_t value) {
    return static_cast<double>(value >> 11U) * 0x1.0p-53;
}

// 1 on flat ground, falling to 0 between two slopes.
double holds(double slope, double from, double to) {
    return 1.0 - smoothstep((slope - from) / (to - from));
}

} // namespace

Layering layering(const Context& context, DVec3 direction, const Column& column, double height,
                  double slope, double temperature) noexcept {
    const Planet& planet = *context.planet;
    const Derived& derived = *context.derived;
    const DVec3 at = direction * planet.radius;
    Layering layers;
    layers.basement = column.high >= 0.5 ? planet.high.rock : planet.low.rock;

    // Only where a volatile has run or stood is rock laid down in beds.
    if (planet.volatiles != Volatile::None) {
        const double wavelength = std::max(basin_wavelength * derived.lateral, 2.0e4);
        const double sag =
            0.5 +
            (0.5 * fbm(at, {.wavelength = wavelength, .gain = 0.5, .finest = wavelength / 4.0}, 0.0,
                       context.seed(Term::Basins))
                       .value);
        const double on_land = (platform_cover * sag) - (0.5 * std::max(0.0, height));
        const double on_floor = planet.regime == Regime::Plates ? ocean_cover : on_land;
        const double cover = (column.high * on_land) + ((1.0 - column.high) * on_floor);
        layers.cover = std::max(0.0, cover * (1.0 - column.built));
        // High ground loses cover as it rises; the basins' sag changes too slowly to count.
        if (layers.cover > 0.0 && height > 0.0) {
            layers.cover_tilt = -0.5 * column.high * (1.0 - column.built) * column.height.gradient;
        }
        const double folding = plain_fold + (belt_fold * column.built);
        const Noise folds =
            fbm(at, {.wavelength = fold_wavelength, .gain = 0.5, .finest = fold_wavelength / 16.0},
                0.0, context.seed(Term::Folds));
        layers.fold = folds.value * folding;
        layers.fold_tilt = tangential(folds.gradient, direction) * folding;
    }

    const bool sea = context.shape->sea;
    const bool underwater = sea && height < 0.0;
    if (derived.liquid) {
        // Weathered where water works; under the sea, mud settles instead.
        layers.top = underwater ? Material::Sediment : Material::Soil;
        layers.top_depth = underwater ? 3.0 : soil_depth * holds(slope, 0.3, 0.8);
        layers.sediment_depth = layers.top_depth + (sediment_depth * holds(slope, 0.05, 0.2));
        if (sea && height > -beach_below && height < beach_above && slope < 0.1) {
            layers.top = Material::Sand;
            layers.top_depth = beach_depth;
        }
    } else {
        const double rate = planet.pressure < 1.0 ? regolith_per_gyr : regolith_per_gyr_under_air;
        layers.top = Material::Regolith;
        layers.top_depth = rate * planet.surface_age * holds(slope, 0.5, 0.9);
        layers.sediment_depth = layers.top_depth;
    }
    // Snow needs water falling as well as cold: only where water cycles through the air.
    if (planet.volatiles == Volatile::Water && derived.liquid && !underwater &&
        temperature < snow_below) {
        layers.snow = snow_depth * holds(slope, 0.6, 1.0);
    }
    return layers;
}

Material material_at(const Context& context, const Layering& layers, double depth,
                     double radius) noexcept {
    if (depth < layers.snow) {
        return Material::Snow;
    }
    if (depth < layers.snow + layers.top_depth) {
        return layers.top;
    }
    if (depth < layers.snow + layers.sediment_depth) {
        return context.derived->liquid ? Material::Sediment : Material::Regolith;
    }
    if (depth >= layers.cover) {
        return layers.basement;
    }
    return bed_rock(context, bed_at(context, layers, radius));
}

std::int64_t bed_at(const Context& context, const Layering& layers, double radius) noexcept {
    const double lifted = radius - context.planet->radius + layers.fold;
    return static_cast<std::int64_t>(std::floor(lifted / bed_thickness));
}

double bed_bottom(const Context& context, const Layering& layers, std::int64_t bed) noexcept {
    return context.planet->radius - layers.fold + (static_cast<double>(bed) * bed_thickness);
}

Material bed_rock(const Context& context, std::int64_t bed) noexcept {
    // Each bed's rock drawn once for the planet, so a bed is the same rock wherever it shows.
    const double draw = unit(hash_values(context.seed(Term::Strata), bed));
    // Limestone is laid down only in seas.
    if (context.shape->sea && draw < 0.3) {
        return Material::Limestone;
    }
    return draw < 0.65 ? Material::Shale : Material::Sandstone;
}

} // namespace hog::globe::terms
