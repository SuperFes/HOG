#include <hog/core/hash.hpp>
#include <hog/planet/noise.hpp>
#include <hog/planet/temperature.hpp>
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

[[nodiscard]] std::int64_t multiply(std::int64_t a, std::int64_t b) {
    return (a * b) >> fixed_fraction_bits;
}

} // namespace

Temperature::Temperature(std::uint64_t seed) : seed_{hash_combine(seed, temperature_salt)} {}

std::int64_t Temperature::at_sea_level(DVec3 at) const {
    // |sin(latitude)|, rounded once from the exact direction.
    const std::int64_t sine = std::llround(std::abs(at.z) * static_cast<double>(fixed_one));
    const std::int64_t cube = multiply(multiply(sine, sine), sine);
    const std::int64_t base =
        equator_temperature - multiply(equator_temperature - pole_temperature, cube);
    const std::int64_t noise =
        multiply(temperature_variation,
                 fbm(seed_, noise_point(at, temperature_frequency), temperature_octaves));
    return base + noise;
}

std::int64_t Temperature::at(DVec3 at, std::int64_t elevation) const {
    const std::int64_t lapse =
        std::max(elevation, std::int64_t{0}) * lapse_numerator / lapse_denominator;
    return at_sea_level(at) - lapse;
}

} // namespace hog::planet
