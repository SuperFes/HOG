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

// What a voxel holds, in fixed point, so it's held, saved and sent the same everywhere: amounts of
// its own volume. Shaped matter (rock, earth and whatever is built), grains that slide and liquids
// that flow. Where a surface lies, and which way it faces, is told by the amounts of the voxels
// about it, nothing else.
struct Voxel {
    static constexpr std::uint8_t sound = 255;
    static constexpr std::uint16_t full = 1U << 15U; // with room over it for matter pressed in

    Material solid = Material::Air;  // what the shaped matter is made of
    std::uint8_t condition = sound;  // how whole it is; nothing is left of it at 0
    Material grain = Material::Air;  // sand, rubble or snow
    std::uint16_t grain_amount = 0;  // of `full`
    Material liquid = Material::Air; // water or lava, filling open space or a solid's pores
    std::uint16_t liquid_amount = 0;
    std::uint16_t temperature = 0; // eighths of a kelvin
    // How much of the voxel's volume is shaped matter, of `full`: all of it deep in the ground,
    // none in open air, between across the surface. The surface lies where it is half.
    std::uint16_t solid_amount = 0;

    friend constexpr bool operator==(const Voxel&, const Voxel&) noexcept = default;
};

// Rounded to the nearest step, clamped to what fits.
[[nodiscard]] std::uint16_t amount_of(double share) noexcept;
[[nodiscard]] std::uint16_t temperature_in_eighths(double kelvin) noexcept;

[[nodiscard]] constexpr double kelvin(std::uint16_t temperature) noexcept {
    return temperature / 8.0;
}

// The solid amount of a voxel `thickness` metres thick that a surface `distance` metres from its
// middle puts in it, negative inside, as a plane across it: half where it passes through the
// middle, all or none a half thickness either way.
[[nodiscard]] std::uint16_t amount_at(double distance, double thickness) noexcept;
// The surface's distance from the voxel's middle that its solid amount says, the other way.
[[nodiscard]] double distance_of(std::uint16_t amount, double thickness) noexcept;

// The voxel a generated sample makes, for a voxel `thickness` metres thick. All the generated
// ground is shaped matter; it turns loose only once something disturbs it.
[[nodiscard]] Voxel voxel_from(const Sample& sample, double thickness) noexcept;

} // namespace hog::globe
