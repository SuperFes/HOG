#pragma once

#include <hog/core/math.hpp>
#include <hog/globe/cell.hpp>
#include <hog/globe/generator.hpp>
#include <hog/globe/layers.hpp>
#include <hog/globe/material.hpp>

#include <compare>
#include <cstdint>

namespace hog::globe {

// A voxel's place: a column of the layers' level and a layer up it.
struct Site {
    std::uint8_t face = 0;
    std::uint32_t i = 0;
    std::uint32_t j = 0;
    std::uint32_t layer = 0;

    friend constexpr auto operator<=>(const Site&, const Site&) noexcept = default;
};

[[nodiscard]] constexpr Cell column_of(Site site, const Layers& layers) noexcept {
    return {.level = static_cast<std::uint8_t>(layers.level),
            .face = site.face,
            .i = site.i,
            .j = site.j};
}

// Where its value is taken: the middle of its column, halfway up its layer.
[[nodiscard]] DVec3 middle_of(Site site, const Layers& layers);

// What a voxel holds, in fixed point, so it's held, saved and sent the same everywhere. Shaped
// matter keeps its form: rock, earth and whatever is built, by its distance to the shaped
// surface. Loose matter is counted: grains that slide and liquids that flow, as amounts of the
// voxel's own volume.
struct Voxel {
    static constexpr std::int16_t far = 32'767; // millimetres, as far as distance is told
    static constexpr std::uint8_t sound = 255;
    static constexpr std::uint16_t full = 1U << 15U; // with room over it for matter pressed in
    static constexpr std::uint16_t no_normal = 0;    // away from any surface

    std::int16_t distance = far;      // millimetres to the shaped surface, negative inside
    std::uint16_t normal = no_normal; // the surface's, out of the matter; packed_normal's
    Material solid = Material::Air;   // what the shaped matter is made of
    std::uint8_t condition = sound;   // how whole it is; nothing is left of it at 0
    Material grain = Material::Air;   // sand, rubble or snow
    std::uint16_t grain_amount = 0;   // of `full`
    Material liquid = Material::Air;  // water or lava, filling open space or a solid's pores
    std::uint16_t liquid_amount = 0;
    std::uint16_t temperature = 0; // eighths of a kelvin
    // How much of the voxel's volume is shaped matter, of `full`: all of it deep in the ground,
    // none in open air, between across the surface. The surface lies where it is half.
    std::uint16_t solid_amount = 0;

    friend constexpr bool operator==(const Voxel&, const Voxel&) noexcept = default;
};

// Rounded to the nearest step, clamped to what fits.
[[nodiscard]] std::int16_t distance_in_mm(double metres) noexcept;
[[nodiscard]] std::uint16_t amount_of(double share) noexcept;
[[nodiscard]] std::uint16_t temperature_in_eighths(double kelvin) noexcept;
// A unit direction to a degree or so, folded onto an octahedron and kept in a byte a side; never
// Voxel::no_normal.
[[nodiscard]] std::uint16_t packed_normal(DVec3 unit) noexcept;
// Unit, or none for Voxel::no_normal.
[[nodiscard]] DVec3 unpacked_normal(std::uint16_t packed) noexcept;

[[nodiscard]] constexpr double metres(std::int16_t distance) noexcept {
    return distance / 1'000.0;
}
[[nodiscard]] constexpr double kelvin(std::uint16_t temperature) noexcept {
    return temperature / 8.0;
}

// How far either side of a surface voxels keep its distance, in metres: beyond, only which side
// they're on, which is all that's drawn there. So a brick all dug out or all rock is one voxel
// throughout.
// Voxels either side of a surface that keep their distance.
inline constexpr double band_voxels = 4.0;
[[nodiscard]] constexpr double distance_band(const Layers& layers) noexcept {
    return band_voxels * layers.thickness;
}
// The same for voxels `coarser` times as far apart.
[[nodiscard]] constexpr double distance_band(const Layers& layers, double coarser) noexcept {
    return distance_band(layers) * coarser;
}

// The voxel a generated sample makes, its distance kept within `band` and its normal only within
// it, so voxels deep in matter or open space are alike. All the generated ground is shaped
// matter; it turns loose only once something disturbs it.
[[nodiscard]] Voxel voxel_from(const Sample& sample, double band) noexcept;

} // namespace hog::globe
