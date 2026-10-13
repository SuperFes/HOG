#include <hog/core/hash.hpp>
#include <hog/planet/moisture.hpp>
#include <hog/planet/noise.hpp>
#include <hog/planet/projection.hpp>
#include <hog/planet/tile.hpp>

#include <algorithm>
#include <cmath>

namespace hog::planet {

namespace {

constexpr std::uint64_t moisture_salt = 0x6D6F'6973'7475'7265ULL; // "moisture"

constexpr std::int64_t band_amplitude = fixed_one / 4;
constexpr std::int64_t inland_dryness = fixed_one / 4;
// Continent height over which the interior dries out.
constexpr std::int64_t inland_dry_width = fixed_one * 3 / 10;
constexpr std::int64_t variation = fixed_one / 4;
constexpr double frequency = 3.0;
constexpr int octaves = 4;

[[nodiscard]] std::int64_t ramp(std::int64_t value, std::int64_t width) {
    return std::clamp((value * fixed_one) / width, std::int64_t{0}, fixed_one);
}

[[nodiscard]] std::int64_t multiply(std::int64_t a, std::int64_t b) {
    return (a * b) >> fixed_fraction_bits;
}

} // namespace

Moisture::Moisture(std::uint64_t seed) : seed_{hash_combine(seed, moisture_salt)} {}

std::int64_t Moisture::at(CellId cell, std::int64_t continent_height) const {
    const DVec3 center = cell_center(cell);
    const std::int64_t sine = std::llround(std::abs(center.z) * static_cast<double>(fixed_one));
    const std::int64_t u = multiply(sine, sine);
    const std::int64_t u2 = multiply(u, u);
    const std::int64_t u3 = multiply(u2, u);
    // cos(6 * latitude): wet at the equator and 60 degrees, dry at 30 degrees and the poles.
    const std::int64_t bands = fixed_one - (18 * u) + (48 * u2) - (32 * u3);
    const std::int64_t noise =
        multiply(variation, fbm(seed_, noise_point(center, frequency), octaves));
    const std::int64_t wetness =
        (fixed_one / 2) + multiply(band_amplitude, bands) -
        multiply(inland_dryness, ramp(continent_height, inland_dry_width)) + noise;
    return std::clamp(wetness, std::int64_t{0}, fixed_one);
}

} // namespace hog::planet
