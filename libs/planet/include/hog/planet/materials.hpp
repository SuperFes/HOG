#pragma once

#include <cstddef>
#include <cstdint>

namespace hog::planet {

// Stored by value, so new materials go on the end.
enum class Material : std::uint8_t {
    Air,
    Water,
    Snow,
    Ice,
    Soil,
    Sediment,
    Sandstone,
    Mudstone,
    Limestone,
    Granite,
    Basalt,
    Metamorphic,
    Magma,
};

inline constexpr std::size_t material_count = static_cast<std::size_t>(Material::Magma) + 1;

struct MaterialProperties {
    float density = 0.0F;       // kilograms per cubic metre
    float specific_heat = 0.0F; // joules per kilogram per kelvin
};

// Typical values near the surface; air at sea level, fresh snow, and melt for magma.
[[nodiscard]] constexpr MaterialProperties properties(Material material) {
    switch (material) {
    case Material::Air:
        return {.density = 1.2F, .specific_heat = 1005.0F};
    case Material::Water:
        return {.density = 1000.0F, .specific_heat = 4186.0F};
    case Material::Snow:
        return {.density = 300.0F, .specific_heat = 2090.0F};
    case Material::Ice:
        return {.density = 917.0F, .specific_heat = 2090.0F};
    case Material::Soil:
        return {.density = 1500.0F, .specific_heat = 800.0F};
    case Material::Sediment:
        return {.density = 1800.0F, .specific_heat = 830.0F};
    case Material::Sandstone:
        return {.density = 2300.0F, .specific_heat = 920.0F};
    case Material::Mudstone:
        return {.density = 2400.0F, .specific_heat = 900.0F};
    case Material::Limestone:
        return {.density = 2600.0F, .specific_heat = 880.0F};
    case Material::Granite:
        return {.density = 2700.0F, .specific_heat = 790.0F};
    case Material::Basalt:
        return {.density = 2900.0F, .specific_heat = 840.0F};
    case Material::Metamorphic:
        return {.density = 2800.0F, .specific_heat = 850.0F};
    case Material::Magma:
        return {.density = 2700.0F, .specific_heat = 1200.0F};
    }
    return {};
}

} // namespace hog::planet
