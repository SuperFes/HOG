#pragma once

#include <hog/globe/material.hpp>
#include <hog/globe/voxel.hpp>

#include <cstdint>

namespace hog::globe {

// What a voxel is when it is a cube and nothing more: what it holds and how full of each, and how
// whole and how hot. Nothing the place alone can say is kept: no distance to a surface, no normal
// and no temperature but heat over what the place gives, so alike voxels are alike bytes and a
// whole cube of them can be one.
struct Cube {
    // Steps a voxel's fullness is told in, from none to full.
    static constexpr int levels = 64;
    // Kelvin a step of heat is, and the least heat over the baseline that is kept.
    static constexpr double heat_step = 8.0;

    Material solid = Material::Air;
    std::uint8_t solid_level = 0;
    Material grain = Material::Air;
    std::uint8_t grain_level = 0;
    Material liquid = Material::Air;
    std::uint8_t liquid_level = 0;
    std::uint8_t condition = Voxel::sound;
    std::uint8_t heat = 0; // steps over the baseline temperature

    friend constexpr bool operator==(const Cube&, const Cube&) noexcept = default;
};

// The cube a voxel comes to, its temperature taken against `baseline` kelvin, what the place
// gives it. A fullness too small to count as a step is none.
[[nodiscard]] Cube cube_of(const Voxel& voxel, double baseline) noexcept;
// The voxel a cube stands for: amounts at the middle of their steps' ends, no normal, a distance
// from the solid amount for a voxel `thickness` metres thick, and a temperature of `baseline` and
// its heat.
[[nodiscard]] Voxel voxel_of(const Cube& cube, double baseline, double thickness) noexcept;

} // namespace hog::globe
