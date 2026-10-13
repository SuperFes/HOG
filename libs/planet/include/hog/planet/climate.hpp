#pragma once

#include <hog/planet/cell_id.hpp>
#include <hog/planet/elevation.hpp>
#include <hog/planet/moisture.hpp>
#include <hog/planet/noise.hpp>
#include <hog/planet/planet_config.hpp>

#include <cstdint>
#include <string_view>

namespace hog::planet {

enum class Biome : std::uint8_t {
    Ocean,
    Beach,
    Ice,
    Tundra,
    BorealForest,
    TemperateForest,
    Grassland,
    Shrubland,
    Desert,
    Savanna,
    TropicalRainforest,
    Alpine,
    Wetland,
    Lake,
};

inline constexpr int biome_count = static_cast<int>(Biome::Lake) + 1;

[[nodiscard]] std::string_view to_string(Biome biome) noexcept;

// Whittaker-style land biome from fixed-point mean annual temperature (°C) and moisture (0 to 1).
[[nodiscard]] constexpr Biome classify_biome(std::int64_t temperature,
                                             std::int64_t moisture) noexcept {
    constexpr std::int64_t ice_below = -12 * fixed_one;
    constexpr std::int64_t tundra_below = -3 * fixed_one;
    constexpr std::int64_t boreal_below = 5 * fixed_one;
    constexpr std::int64_t temperate_below = 20 * fixed_one;

    constexpr std::int64_t boreal_moisture = fixed_one * 3 / 10;
    constexpr std::int64_t temperate_desert_moisture = fixed_one * 3 / 20;
    constexpr std::int64_t grassland_moisture = fixed_one * 3 / 10;
    constexpr std::int64_t shrubland_moisture = fixed_one * 9 / 25;
    constexpr std::int64_t tropical_desert_moisture = fixed_one / 5;
    constexpr std::int64_t savanna_moisture = fixed_one * 11 / 20;

    if (temperature < ice_below) {
        return Biome::Ice;
    }
    if (temperature < tundra_below) {
        return Biome::Tundra;
    }
    if (temperature < boreal_below) {
        return moisture < boreal_moisture ? Biome::Tundra : Biome::BorealForest;
    }
    if (temperature < temperate_below) {
        if (moisture < temperate_desert_moisture) {
            return Biome::Desert;
        }
        if (moisture < grassland_moisture) {
            return Biome::Grassland;
        }
        return moisture < shrubland_moisture ? Biome::Shrubland : Biome::TemperateForest;
    }
    if (moisture < tropical_desert_moisture) {
        return Biome::Desert;
    }
    return moisture < savanna_moisture ? Biome::Savanna : Biome::TropicalRainforest;
}

// Climate from latitude and terrain: temperature falls towards the poles and with height;
// moisture follows Earth-like circulation bands and dries out deep inland. High ground cold enough
// holds glaciers, wet ground where the water table reaches the surface turns to wetland, and
// ground under a lake is lake.
class Climate {
public:
    // Metres.
    static constexpr std::int64_t alpine_height = 1'800;
    static constexpr std::int64_t beach_height = 10;
    static constexpr std::int64_t wetland_depth = 1;

    explicit Climate(const PlanetConfig& config);

    [[nodiscard]] const Elevation& terrain() const noexcept { return terrain_; }

    // Fixed-point mean annual temperature in °C, at the surface or the sea surface.
    [[nodiscard]] std::int64_t temperature(CellId cell) const;

    // Fixed-point wetness from 0 (arid) to 1 (saturated). Only continents dry out inland; islands
    // are too small to.
    [[nodiscard]] std::int64_t moisture(CellId cell) const;

    // Fixed-point metres from the ground down to the water table: zero at sea, shallow near water
    // and in wet climates, deep under high, dry ground.
    [[nodiscard]] std::int64_t water_table(CellId cell) const;

    [[nodiscard]] Biome biome(CellId cell) const;

private:
    [[nodiscard]] std::int64_t temperature(CellId cell, std::int64_t elevation) const;
    [[nodiscard]] std::int64_t water_table(CellId cell, std::int64_t moisture) const;

    Elevation terrain_;
    std::uint64_t temperature_seed_;
    Moisture moisture_;
};

} // namespace hog::planet
