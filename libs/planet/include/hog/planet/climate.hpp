#pragma once

#include <hog/planet/cell_id.hpp>
#include <hog/planet/elevation.hpp>
#include <hog/planet/moisture.hpp>
#include <hog/planet/noise.hpp>
#include <hog/planet/planet_config.hpp>
#include <hog/planet/temperature.hpp>

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

// A land biome and the one its climate shades into across the nearest boundary between them,
// with that one's share in fixed point: half on the boundary, falling to none a few degrees or a
// few hundredths of wetness from it, as species' tolerances spread about the climate they suit.
struct BiomeBlend {
    Biome biome = Biome::Ocean;
    Biome other = Biome::Ocean;
    std::int64_t share = 0;
};

// For fixed-point °C and wetness, as classify_biome() takes them.
[[nodiscard]] BiomeBlend blend_biome(std::int64_t temperature, std::int64_t moisture) noexcept;

// The year's noon sun on ground at the unit direction `at` sloping up `uphill`, as Landform has
// it, against on level ground there: under 1 on slopes facing the nearer pole, over on those
// facing the equator.
[[nodiscard]] double sunlit(DVec3 at, DVec3 uphill);

// Fixed point from 0 (arid) to 1: how wet plants find ground given fixed-point millimetres of
// rain a year and °C, `sunlit` as above and fixed-point metres `above_water`. Sunlit slopes
// evaporate more and shaded ones less, and roots reaching the water table never go dry.
[[nodiscard]] std::int64_t plant_wetness(std::int64_t rain, std::int64_t temperature, double sunlit,
                                         std::int64_t above_water);

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
    // The same, for a cell whose fixed-point elevation is already known.
    [[nodiscard]] std::int64_t temperature(CellId cell, std::int64_t elevation) const;

    // Fixed-point wetness from 0 (arid) to 1 (saturated). Only continents dry out inland; islands
    // are too small to. Where the winds bring the rain, as plants find it on the ground.
    [[nodiscard]] std::int64_t moisture(CellId cell) const;

    // Fixed-point metres from the ground down to the water table: zero at sea, shallow near water
    // and in wet climates, deep under high, dry ground. The climate's wetness sets it, whatever
    // the slope.
    [[nodiscard]] std::int64_t water_table(CellId cell) const;

    [[nodiscard]] Biome biome(CellId cell) const;

    // A cell's surface climate, read in one pass.
    struct Surface {
        Biome biome = Biome::Ocean;
        std::int64_t temperature = 0; // as temperature()
        std::int64_t moisture = 0;    // as moisture(); saturated under water
        std::int64_t water_table = 0; // as water_table(); zero under water
    };

    // For a cell whose fixed-point elevation is already known.
    [[nodiscard]] Surface surface(CellId cell, std::int64_t elevation) const;

private:
    [[nodiscard]] std::int64_t water_table(CellId cell, std::int64_t moisture) const;
    // The climate's wetness at a cell this many fixed-point °C warm, before the ground has a say.
    [[nodiscard]] std::int64_t climatic(CellId cell, std::int64_t temperature) const;
    // As moisture(), given the cell's temperature and its wetness by the climate.
    [[nodiscard]] std::int64_t moisture(CellId cell, std::int64_t temperature,
                                        std::int64_t climatic) const;

    Elevation terrain_;
    Temperature temperature_;
    Moisture moisture_;
};

} // namespace hog::planet
