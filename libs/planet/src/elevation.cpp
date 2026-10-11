#include <hog/core/hash.hpp>
#include <hog/planet/elevation.hpp>
#include <hog/planet/noise.hpp>
#include <hog/planet/projection.hpp>
#include <hog/planet/tile.hpp>

#include <algorithm>

namespace hog::planet {

namespace {

constexpr std::uint64_t belt_salt = 0x6265'6C74'7300'0000ULL;  // "belts"
constexpr std::uint64_t ridge_salt = 0x7269'6467'6573'0000ULL; // "ridges"

// Continent heights (fixed point) over which each effect ramps from nothing to full.
constexpr std::int64_t depth_width = fixed_one * 3 / 20;
constexpr std::int64_t lowland_width = fixed_one / 5;
constexpr std::int64_t inland_width = fixed_one / 10;

constexpr double belt_frequency = 2.0;
constexpr int belt_octaves = 3;
constexpr std::int64_t belt_threshold = fixed_one / 8;
constexpr std::int64_t belt_width = fixed_one / 5;

constexpr double ridge_frequency = 6.0;
constexpr int ridge_octaves = 6;
constexpr std::int64_t erosion = 2 * fixed_one;

// 0 at or below zero, rising linearly to 1 at `width`.
[[nodiscard]] std::int64_t ramp(std::int64_t value, std::int64_t width) {
    return std::clamp((value * fixed_one) / width, std::int64_t{0}, fixed_one);
}

} // namespace

Elevation::Elevation(std::uint64_t seed, double ocean_fraction)
    : continents_{seed, ocean_fraction}, belt_seed_{hash_combine(seed, belt_salt)},
      ridge_seed_{hash_combine(seed, ridge_salt)} {}

std::int64_t Elevation::mountain_strength(CellId cell) const {
    return mountain_strength(cell, continents_.height(cell));
}

std::int64_t Elevation::mountain_strength(CellId cell, std::int64_t height) const {
    const std::int64_t inland = ramp(height, inland_width);
    if (inland == 0) {
        return 0;
    }
    const std::int64_t belt =
        ramp(fbm(belt_seed_, noise_point(cell_center(cell), belt_frequency), belt_octaves) -
                 belt_threshold,
             belt_width);
    return (inland * belt) >> fixed_fraction_bits;
}

std::int64_t Elevation::elevation(CellId cell) const {
    const std::int64_t height = continents_.height(cell);
    if (height <= 0) {
        return -ocean_depth * ramp(-height, depth_width);
    }
    const std::int64_t lowland = lowland_height * ramp(height, lowland_width);
    const std::int64_t strength = mountain_strength(cell, height);
    if (strength == 0) {
        return lowland;
    }
    const std::int64_t ridge = ridges(ridge_seed_, noise_point(cell_center(cell), ridge_frequency),
                                      ridge_octaves, erosion);
    return lowland + (mountain_height * ((strength * ridge) >> fixed_fraction_bits));
}

} // namespace hog::planet
