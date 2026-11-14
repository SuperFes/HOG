#pragma once

#include <hog/core/math.hpp>
#include <hog/globe/layers.hpp>
#include <hog/globe/material.hpp>
#include <hog/globe/planet.hpp>
#include <hog/globe/voxel.hpp>

#include <cstdint>

namespace hog::globe {

// What a voxel is when it is a cube and nothing more: what it holds and how much of each, and how
// whole and how hot. Nothing the place alone can say is kept: no distance to a surface, no normal
// and no temperature but heat over what the place gives, so alike voxels are alike bytes and a
// whole cube of them can be one. Amounts are the voxel's own, so what flows is never lost.
struct Cube {
    // Kelvin a step of heat is, and the least heat over the baseline that is kept.
    static constexpr double heat_step = 8.0;

    Material solid = Material::Air;
    std::uint16_t solid_amount = 0;
    Material grain = Material::Air;
    std::uint16_t grain_amount = 0;
    Material liquid = Material::Air;
    std::uint16_t liquid_amount = 0;
    std::uint8_t condition = Voxel::sound;
    std::uint8_t heat = 0; // steps over the baseline temperature
    std::uint8_t rise = 0; // as Voxel's, none where it holds no liquid
    std::uint8_t peak = 0;

    friend constexpr bool operator==(const Cube&, const Cube&) noexcept = default;
};

// The cube a voxel comes to, its temperature taken against `baseline` kelvin, what the place
// gives it. A material with none of it is none.
[[nodiscard]] Cube cube_of(const Voxel& voxel, double baseline) noexcept;
// The voxel a cube stands for, at a temperature of `baseline` and its heat.
[[nodiscard]] Voxel voxel_of(const Cube& cube, double baseline) noexcept;

// What the place gives a voxel, the baseline its heat is told over: the open air's mean
// temperature at its middle. Ground and water are never colder than the air at their height.
[[nodiscard]] double place_temperature(const Planet& planet, const Layers& layers, Site site);
// The same `radius` metres out along `up`, a unit direction.
[[nodiscard]] double place_temperature(const Planet& planet, DVec3 up, double radius);

} // namespace hog::globe
