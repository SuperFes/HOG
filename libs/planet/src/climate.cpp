#include <hog/core/hash.hpp>
#include <hog/planet/climate.hpp>
#include <hog/planet/projection.hpp>
#include <hog/planet/tile.hpp>

#include <algorithm>
#include <cmath>

namespace hog::planet {

namespace {

constexpr std::uint64_t temperature_salt = 0x7465'6D70'6572'6174ULL; // "temperat"
constexpr std::uint64_t moisture_salt = 0x6D6F'6973'7475'7265ULL;    // "moisture"

constexpr std::int64_t equator_temperature = 28 * fixed_one;
constexpr std::int64_t pole_temperature = -25 * fixed_one;
// 6.5 °C per kilometre.
constexpr std::int64_t lapse_numerator = 13;
constexpr std::int64_t lapse_denominator = 2'000;
constexpr std::int64_t temperature_variation = 4 * fixed_one;
constexpr double temperature_frequency = 2.0;
constexpr int temperature_octaves = 4;

constexpr std::int64_t band_amplitude = fixed_one / 4;
constexpr std::int64_t inland_dryness = fixed_one / 4;
// Continent height over which the interior dries out.
constexpr std::int64_t inland_dry_width = fixed_one * 3 / 10;
constexpr std::int64_t moisture_variation = fixed_one / 4;
constexpr double moisture_frequency = 3.0;
constexpr int moisture_octaves = 4;

constexpr std::int64_t beach_temperature = 0;

[[nodiscard]] std::int64_t ramp(std::int64_t value, std::int64_t width) {
    return std::clamp((value * fixed_one) / width, std::int64_t{0}, fixed_one);
}

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
    }
    return "unknown";
}

Climate::Climate(std::uint64_t seed, double ocean_fraction)
    : terrain_{seed, ocean_fraction}, temperature_seed_{hash_combine(seed, temperature_salt)},
      moisture_seed_{hash_combine(seed, moisture_salt)} {}

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
    return moisture(cell, terrain_.continents().height(cell));
}

std::int64_t Climate::moisture(CellId cell, std::int64_t height) const {
    const DVec3 center = cell_center(cell);
    const std::int64_t sine = latitude_sine(center);
    const std::int64_t u = multiply(sine, sine);
    const std::int64_t u2 = multiply(u, u);
    const std::int64_t u3 = multiply(u2, u);
    // cos(6 * latitude): wet at the equator and 60 degrees, dry at 30 degrees and the poles.
    const std::int64_t bands = fixed_one - (18 * u) + (48 * u2) - (32 * u3);
    const std::int64_t noise =
        multiply(moisture_variation,
                 fbm(moisture_seed_, noise_point(center, moisture_frequency), moisture_octaves));
    const std::int64_t wetness = (fixed_one / 2) + multiply(band_amplitude, bands) -
                                 multiply(inland_dryness, ramp(height, inland_dry_width)) + noise;
    return std::clamp(wetness, std::int64_t{0}, fixed_one);
}

Biome Climate::biome(CellId cell) const {
    const std::int64_t height = terrain_.continents().height(cell);
    if (height <= 0) {
        return Biome::Ocean;
    }
    const std::int64_t elevation = terrain_.elevation(cell);
    const std::int64_t temperature = this->temperature(cell, elevation);
    const Biome climate = classify_biome(temperature, moisture(cell, height));
    if (climate == Biome::Ice) {
        return climate;
    }
    if (elevation > alpine_height * fixed_one) {
        return Biome::Alpine;
    }
    if (elevation < beach_height * fixed_one && temperature > beach_temperature) {
        return Biome::Beach;
    }
    return climate;
}

} // namespace hog::planet
