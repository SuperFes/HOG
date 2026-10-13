#include <hog/core/hash.hpp>
#include <hog/planet/climate.hpp>
#include <hog/planet/projection.hpp>
#include <hog/planet/tile.hpp>

#include <algorithm>
#include <cmath>

namespace hog::planet {

namespace {

constexpr std::uint64_t temperature_salt = 0x7465'6D70'6572'6174ULL; // "temperat"

constexpr std::int64_t equator_temperature = 28 * fixed_one;
constexpr std::int64_t pole_temperature = -25 * fixed_one;
// 6.5 °C per kilometre.
constexpr std::int64_t lapse_numerator = 13;
constexpr std::int64_t lapse_denominator = 2'000;
constexpr std::int64_t temperature_variation = 4 * fixed_one;
constexpr double temperature_frequency = 2.0;
constexpr int temperature_octaves = 4;

constexpr std::int64_t beach_temperature = 0;
// High ground colder than this through the year keeps its snow as glaciers.
constexpr std::int64_t glacier_temperature = -4 * fixed_one;
// The water table sits at this share of the height above water in a saturated climate, rising to
// the full height and beyond in a dry one.
constexpr std::int64_t wet_table_share = fixed_one / 5;
constexpr std::int64_t dry_table_share = fixed_one * 6 / 5;

[[nodiscard]] std::int64_t multiply(std::int64_t a, std::int64_t b) {
    return (a * b) >> fixed_fraction_bits;
}

// |sin(latitude)| in fixed point, rounded once from the exact cell centre.
[[nodiscard]] std::int64_t latitude_sine(DVec3 center) {
    return std::llround(std::abs(center.z) * static_cast<double>(fixed_one));
}

} // namespace

std::string_view to_string(Biome biome) noexcept {
    switch (biome) {
    case Biome::Ocean:
        return "ocean";
    case Biome::Beach:
        return "beach";
    case Biome::Ice:
        return "ice";
    case Biome::Tundra:
        return "tundra";
    case Biome::BorealForest:
        return "boreal forest";
    case Biome::TemperateForest:
        return "temperate forest";
    case Biome::Grassland:
        return "grassland";
    case Biome::Shrubland:
        return "shrubland";
    case Biome::Desert:
        return "desert";
    case Biome::Savanna:
        return "savanna";
    case Biome::TropicalRainforest:
        return "tropical rainforest";
    case Biome::Alpine:
        return "alpine";
    case Biome::Wetland:
        return "wetland";
    }
    return "unknown";
}

Climate::Climate(const PlanetConfig& config)
    : terrain_{config}, temperature_seed_{hash_combine(config.seed, temperature_salt)},
      moisture_{config.seed} {}

std::int64_t Climate::temperature(CellId cell) const {
    return temperature(cell, terrain_.elevation(cell));
}

std::int64_t Climate::temperature(CellId cell, std::int64_t elevation) const {
    const DVec3 center = cell_center(cell);
    const std::int64_t sine = latitude_sine(center);
    const std::int64_t cube = multiply(multiply(sine, sine), sine);
    const std::int64_t base =
        equator_temperature - multiply(equator_temperature - pole_temperature, cube);
    const std::int64_t lapse =
        std::max(elevation, std::int64_t{0}) * lapse_numerator / lapse_denominator;
    const std::int64_t noise = multiply(
        temperature_variation,
        fbm(temperature_seed_, noise_point(center, temperature_frequency), temperature_octaves));
    return base - lapse + noise;
}

std::int64_t Climate::moisture(CellId cell) const {
    return moisture_.at(cell, terrain_.continents().height(cell));
}

std::int64_t Climate::water_table(CellId cell) const {
    return water_table(cell, moisture(cell));
}

std::int64_t Climate::water_table(CellId cell, std::int64_t moisture) const {
    const std::int64_t share =
        dry_table_share - multiply(dry_table_share - wet_table_share, moisture);
    return multiply(terrain_.above_water(cell), share);
}

Biome Climate::biome(CellId cell) const {
    const std::int64_t continent = terrain_.continents().height(cell);
    if (terrain_.archipelago().raise(cell, continent) <= 0) {
        return Biome::Ocean;
    }
    const std::int64_t elevation = terrain_.elevation(cell);
    const std::int64_t temperature = this->temperature(cell, elevation);
    const std::int64_t moisture = moisture_.at(cell, continent);
    const Biome climate = classify_biome(temperature, moisture);
    if (climate == Biome::Ice) {
        return climate;
    }
    if (elevation > alpine_height * fixed_one) {
        return temperature < glacier_temperature ? Biome::Ice : Biome::Alpine;
    }
    if (elevation < beach_height * fixed_one && temperature > beach_temperature) {
        return Biome::Beach;
    }
    if (climate != Biome::Desert && water_table(cell, moisture) < wetland_depth * fixed_one) {
        return Biome::Wetland;
    }
    return climate;
}

} // namespace hog::planet
