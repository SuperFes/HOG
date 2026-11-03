#pragma once

#include <hog/core/hash.hpp>
#include <hog/globe/material.hpp>
#include <hog/globe/surface.hpp>
#include <hog/globe/voxel.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <optional>
#include <vector>

namespace hog::flow {

// What the sky gives the ground over a place.
struct Sky {
    double rain = 0.0;    // mm an hour falling as rain; none where it falls as snow
    double dryness = 0.0; // 0 for saturated air to 1 for none: how much water it takes back
};

// A voxel's liquid, to be set.
struct Soaking {
    globe::Site site;
    std::uint16_t amount = 0;
    globe::Material liquid = globe::Material::Air;
};

// Rain onto the ground and the air drying it, told as changes to the voxels' own liquid, which
// is what every part of the world already reads wetness from. Rain soaks into the top of the
// ground at what the ground lets through; what falls faster lies on it as a film for the flow to
// carry off, and the film and then the ground's water are what the air takes.
class Wetting {
public:
    // Millimetres an hour the air takes from wet ground at 20 °C in air as dry as it gets.
    static constexpr double drying_peak = 0.3;
    // Layers looked down for the ground, and kelvin of warmth that doubles as much as 20.
    static constexpr std::uint32_t reach = 4;
    static constexpr double drying_kelvin = 20.0;

    // The changes `sky` makes to the ground below layer `start` of a column over `seconds`, at
    // most two: the ground's top voxel and the film over it. `read(layer)` gives that column's
    // voxel there. `draw` varies which units a fractional amount rounds up, so a drizzle too
    // fine for one step still falls.
    template <typename Read>
    [[nodiscard]] static std::vector<Soaking> column(const Read& read, globe::Site column,
                                                     std::uint32_t start, double thickness,
                                                     Sky sky, double seconds, std::uint64_t draw);
};

namespace detail {

[[nodiscard]] inline std::int64_t rounded(double value, std::uint64_t draw) noexcept {
    const double whole = std::floor(value);
    const double unit = static_cast<double>(draw >> 11U) * 0x1.0p-53;
    return static_cast<std::int64_t>(whole) + (unit < value - whole ? 1 : 0);
}

} // namespace detail

template <typename Read>
std::vector<Soaking> Wetting::column(const Read& read, globe::Site column, std::uint32_t start,
                                     double thickness, Sky sky, double seconds,
                                     std::uint64_t draw) {
    using globe::Material;
    using globe::Voxel;

    std::array<Voxel, reach + 3> met{};
    const auto lookup = [&](std::uint32_t layer) -> const Voxel* {
        const std::optional<Voxel> found = read(layer);
        if (!found) {
            return nullptr;
        }
        Voxel& slot = met[layer % met.size()];
        slot = *found;
        return &slot;
    };
    const std::optional<globe::Surface> surface =
        globe::surface_below(start, reach, 1, thickness, lookup);
    if (!surface || surface->first == 0) {
        return {};
    }
    const globe::Site bed_at{.face = column.face,
                             .i = column.i,
                             .j = column.j,
                             .layer = surface->first - 1};
    globe::Site film_at = bed_at;
    ++film_at.layer;
    Voxel bed = *lookup(bed_at.layer);
    Voxel film = surface->bottom;
    // Only water wets or dries; lava or another liquid in the ground is left alone.
    const auto water = [](const Voxel& voxel) {
        return voxel.liquid == Material::Air || voxel.liquid == Material::Water;
    };
    if (!water(bed) || !water(film)) {
        return {};
    }

    const std::int64_t before_bed = bed.liquid_amount;
    const std::int64_t before_film = film.liquid_amount;
    const auto units = [&](double mm_an_hour, std::uint64_t salt) {
        const double metres = mm_an_hour / 1000.0 * seconds / 3600.0;
        return detail::rounded(metres / thickness * Voxel::full,
                               hash_values(draw, salt, std::uint64_t{column.i},
                                           std::uint64_t{column.j}, std::uint64_t{column.face}));
    };
    std::int64_t bed_amount = bed.liquid_amount;
    std::int64_t film_amount = film.liquid_amount;

    if (sky.rain > 0.0) {
        std::int64_t fallen = units(sky.rain, 1);
        const std::int64_t pores =
            globe::amount_of(globe::properties(bed.solid).porosity) - bed_amount;
        const double through = globe::properties(bed.solid).conductivity * seconds / thickness *
                               static_cast<double>(Voxel::full);
        const std::int64_t soaked = std::clamp<std::int64_t>(
            std::min<std::int64_t>(fallen, detail::rounded(through, hash_values(draw, 2U))), 0,
            std::max<std::int64_t>(pores, 0));
        bed_amount += soaked;
        fallen -= soaked;
        film_amount =
            std::min<std::int64_t>(film_amount + fallen, Voxel::full - film.grain_amount);
    }
    // Water standing deeper than a film is a pond or the sea, which the air takes too slowly for
    // here to matter and the ground under it stays soaked.
    const bool standing = static_cast<double>(film_amount) * thickness > 0.05 * Voxel::full;
    if (sky.dryness > 0.0 && !standing) {
        const double warmth = std::clamp((globe::kelvin(bed.temperature) - 273.15) / drying_kelvin,
                                         0.0, 1.5);
        std::int64_t taken = units(drying_peak * sky.dryness * warmth, 3);
        const std::int64_t from_film = std::min(taken, film_amount);
        film_amount -= from_film;
        taken -= from_film;
        bed_amount -= std::min(taken, bed_amount);
    }

    std::vector<Soaking> changes;
    if (bed_amount != before_bed) {
        changes.push_back({.site = bed_at,
                           .amount = static_cast<std::uint16_t>(bed_amount),
                           .liquid = bed_amount > 0 ? Material::Water : Material::Air});
    }
    if (film_amount != before_film) {
        changes.push_back({.site = film_at,
                           .amount = static_cast<std::uint16_t>(film_amount),
                           .liquid = film_amount > 0 ? Material::Water : Material::Air});
    }
    return changes;
}

} // namespace hog::flow
