#include <hog/core/hash.hpp>
#include <hog/planet/elevation.hpp>
#include <hog/planet/moisture.hpp>
#include <hog/planet/noise.hpp>
#include <hog/planet/projection.hpp>
#include <hog/planet/tile.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <map>
#include <mutex>
#include <tuple>
#include <vector>

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

// Rivers wear the land for 5 million years in steps of 100,000, on cells about this wide, or as
// fine as the time to evolve them allows on large planets.
constexpr double landform_cell = 1'500.0; // metres
constexpr int landform_level = 8;
constexpr int landform_steps = 50;
// Metres per step: 0.2 mm a year in the heart of a mountain belt, and a slow rise everywhere.
constexpr double mountain_uplift = 20.0;
constexpr double background_uplift = 1.0;
// The stream-power K over a step (5e-7 a year); a belt's granite resists half as much again.
constexpr double erodibility = 0.05;
constexpr double granite_hardness = 0.5;
// Generated height over which the change fades in from the shore.
constexpr std::int64_t coast_blend = 50 * fixed_one;

// 0 at or below zero, rising linearly to 1 at `width`.
[[nodiscard]] std::int64_t ramp(std::int64_t value, std::int64_t width) {
    return std::clamp((value * fixed_one) / width, std::int64_t{0}, fixed_one);
}

} // namespace

Elevation::Elevation(const PlanetConfig& config)
    : continents_{config.seed, config.ocean_fraction},
      archipelago_{config.seed, config.radius, continents_},
      belt_seed_{hash_combine(config.seed, belt_salt)},
      ridge_seed_{hash_combine(config.seed, ridge_salt)},
      evolving_{std::make_shared<Evolving>(config)} {}

const Landform& Elevation::landform() const {
    std::call_once(evolving_->once, [this] { evolving_->landform = evolve(evolving_->config); });
    return *evolving_->landform;
}

std::shared_ptr<const Landform> Elevation::evolve(const PlanetConfig& config) const {
    // Evolving takes a while, so it's shared while any Elevation of the same planet lives.
    using Key = std::tuple<std::uint64_t, std::uint64_t, std::uint64_t>;
    static std::mutex mutex;
    static std::map<Key, std::weak_ptr<const Landform>> evolved;
    const Key key{config.seed, std::bit_cast<std::uint64_t>(config.radius),
                  std::bit_cast<std::uint64_t>(config.ocean_fraction)};
    const std::scoped_lock lock{mutex};
    if (auto shared = evolved[key].lock()) {
        return shared;
    }

    const auto fixed = [](double value) {
        return std::llround(value * static_cast<double>(fixed_one));
    };
    LevelGrid grid{std::min(level_for(landform_cell, config.radius), landform_level),
                   config.radius};
    const Moisture moisture{config.seed};
    std::vector<std::int64_t> heights(grid.size());
    std::vector<std::int64_t> uplift(grid.size());
    std::vector<std::int64_t> hardness(grid.size());
    std::vector<std::int64_t> runoff(grid.size());
    for (std::uint32_t index = 0; index < grid.size(); ++index) {
        const CellId cell = grid.cell(index);
        const double strength = fixed_to_double(mountain_strength(cell));
        heights[index] = generated(cell);
        uplift[index] = fixed(background_uplift + (mountain_uplift * strength));
        hardness[index] = fixed(erodibility * (1.0 - (granite_hardness * strength)));
        runoff[index] = moisture.at(cell, continents_.height(cell));
    }
    auto landform = std::make_shared<const Landform>(
        std::move(grid), std::move(heights),
        Erosion{.uplift = uplift, .erodibility = hardness, .runoff = runoff}, landform_steps);
    evolved[key] = landform;
    return landform;
}

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
    const std::int64_t generated = this->generated(cell);
    if (generated <= 0) {
        return generated;
    }
    // The change fades out towards the shore, and low ground is never worn below a quarter of
    // its height, so coasts stay where generated and meet the sea without a step.
    const std::int64_t change =
        (landform().change(cell) * ramp(generated, coast_blend)) >> fixed_fraction_bits;
    return std::max(generated + change, (generated + 3) / 4);
}

std::int64_t Elevation::generated(CellId cell) const {
    const std::int64_t continent = continents_.height(cell);
    const std::int64_t height = archipelago_.raise(cell, continent);
    if (height <= 0) {
        return -ocean_depth * ramp(-height, depth_width);
    }
    const std::int64_t lowland = lowland_height * ramp(height, lowland_width);
    // Islands keep to lowland hills; only continents carry mountain belts.
    const std::int64_t strength = mountain_strength(cell, continent);
    if (strength == 0) {
        return lowland;
    }
    const std::int64_t ridge = ridges(ridge_seed_, noise_point(cell_center(cell), ridge_frequency),
                                      ridge_octaves, erosion);
    return lowland + (mountain_height * ((strength * ridge) >> fixed_fraction_bits));
}

} // namespace hog::planet
