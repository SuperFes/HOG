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

constexpr std::uint64_t belt_salt = 0x6265'6C74'7300'0000ULL;   // "belts"
constexpr std::uint64_t ridge_salt = 0x7269'6467'6573'0000ULL;  // "ridges"
constexpr std::uint64_t relief_salt = 0x7265'6C69'6566'0000ULL; // "relief"

// Continent heights (fixed point) over which each effect ramps from nothing to full.
template <int Bits>
constexpr std::int64_t depth_width = fixed_unit<Bits> * 3 / 20;
template <int Bits>
constexpr std::int64_t lowland_width = fixed_unit<Bits> / 5;
template <int Bits>
constexpr std::int64_t inland_width = fixed_unit<Bits> / 10;

constexpr double belt_frequency = 2.0;
constexpr int belt_octaves = 3;
template <int Bits>
constexpr std::int64_t belt_threshold = fixed_unit<Bits> / 8;
template <int Bits>
constexpr std::int64_t belt_width = fixed_unit<Bits> / 5;

// Continental lowlands roll by some tens of metres over a dozen kilometres or so, so no plain is
// dead flat and water always finds a way down.
constexpr std::int64_t relief_height = 80;
constexpr double relief_frequency = 8.0;
constexpr int relief_octaves = 4;

constexpr double ridge_frequency = 6.0;
constexpr int ridge_octaves = 6;
template <int Bits>
constexpr std::int64_t erosion = 2 * fixed_unit<Bits>;

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

// Off a coast, height below the sea is read as metres from the shore as the islands' flanks fall;
// continents' coasts fall about twice as fast on a small planet.
constexpr std::int64_t shore_metres = 100'000;
static_assert(static_cast<double>(shore_metres) * Archipelago::skirt_slope > 0.999'999 &&
              static_cast<double>(shore_metres) * Archipelago::skirt_slope < 1.000'001);
// Fine sand's A in Dean's h = A·x^(2/3), in m^(1/3) with 16 fractional bits.
constexpr std::int64_t dean_scale = (12 * fixed_one) / 100;
// Metres out per metre down across the shelf, and how far out it breaks.
constexpr std::int64_t shelf_run = 100;
constexpr std::int64_t shelf_break = 5'000;

// 0 at or below zero, rising linearly to 1 at `width`.
template <int Bits = fixed_fraction_bits>
[[nodiscard]] std::int64_t ramp(std::int64_t value, std::int64_t width) {
    return std::clamp((value * fixed_unit<Bits>) / width, std::int64_t{0}, fixed_unit<Bits>);
}

} // namespace

Elevation::Elevation(const PlanetConfig& config)
    : continents_{config.seed, config.ocean_fraction},
      archipelago_{config.seed, config.radius, continents_},
      belt_seed_{hash_combine(config.seed, belt_salt)},
      ridge_seed_{hash_combine(config.seed, ridge_salt)},
      relief_seed_{hash_combine(config.seed, relief_salt)},
      evolving_{std::make_shared<Evolving>(config)} {}

Elevation::Evolved::Evolved(Landform worn, const Elevation& terrain, const PlanetConfig& config)
    : landform{std::move(worn)}, meander{config.seed, landform.grid()}, rivers{[&] {
          std::vector<std::int64_t> ground(landform.grid().size());
          for (std::uint32_t index = 0; index < landform.grid().size(); ++index) {
              const CellId cell = landform.grid().cell(index);
              ground[index] = Elevation::worn(cell, terrain.generated(cell), landform, meander);
          }
          return Rivers{landform, ground, meander, config.radius, config.river_scale};
      }()} {}

const Elevation::Evolved& Elevation::evolved() const {
    std::call_once(evolving_->once, [this] { evolving_->evolved = evolve(evolving_->config); });
    return *evolving_->evolved;
}

std::shared_ptr<const Elevation::Evolved> Elevation::evolve(const PlanetConfig& config) const {
    // Evolving takes a while, so it's shared while any Elevation of the same planet lives.
    using Key = std::tuple<std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t>;
    static std::mutex mutex;
    static std::map<Key, std::weak_ptr<const Evolved>> evolved;
    const Key key{config.seed, std::bit_cast<std::uint64_t>(config.radius),
                  std::bit_cast<std::uint64_t>(config.ocean_fraction),
                  std::bit_cast<std::uint64_t>(config.river_scale)};
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
    auto shared = std::make_shared<const Evolved>(
        Landform{std::move(grid), std::move(heights),
                 Erosion{.uplift = uplift, .erodibility = hardness, .runoff = runoff},
                 landform_steps},
        *this, config);
    evolved[key] = shared;
    return shared;
}

std::int64_t Elevation::mountain_strength(CellId cell) const {
    // As strong as the generated ground has it.
    constexpr int finer = fine_fraction_bits - fixed_fraction_bits;
    return mountain_strength<fine_fraction_bits>(cell,
                                                 continents_.height<fine_fraction_bits>(cell)) >>
           finer;
}

template <int Bits>
std::int64_t Elevation::mountain_strength(CellId cell, std::int64_t height) const {
    const std::int64_t inland = ramp<Bits>(height, inland_width<Bits>);
    if (inland == 0) {
        return 0;
    }
    const std::int64_t belt = ramp<Bits>(
        fbm(belt_seed_, noise_point<Bits>(cell_center(cell), belt_frequency), belt_octaves) -
            belt_threshold<Bits>,
        belt_width<Bits>);
    return (inland * belt) >> Bits;
}

std::int64_t Elevation::elevation(CellId cell) const {
    const std::int64_t generated = this->generated(cell);
    if (generated <= 0) {
        return generated;
    }
    const Evolved& evolved = this->evolved();
    const std::int64_t ground = worn(cell, generated, evolved.landform, evolved.meander);
    return evolved.rivers.cut(cell, ground);
}

std::optional<Channel> Elevation::river(CellId cell) const {
    const std::int64_t generated = this->generated(cell);
    if (generated <= 0) {
        return std::nullopt;
    }
    const Evolved& evolved = this->evolved();
    return evolved.rivers.channel(cell, worn(cell, generated, evolved.landform, evolved.meander));
}

bool Elevation::is_land(CellId cell) const {
    if (height<fine_fraction_bits>(cell) > 0) {
        return true;
    }
    // At or below sea level, told by the fine height, so ground right at it is the sea's.
    const auto water = evolved().rivers.standing(cell, std::min<std::int64_t>(generated(cell), -1));
    return !water || !water->sea;
}

std::optional<Channel> Elevation::lake(CellId cell) const {
    const std::int64_t ground = elevation(cell);
    const auto level = evolved().rivers.lake(cell, ground);
    if (!level) {
        return std::nullopt;
    }
    return Channel{.bed = ground, .surface = *level};
}

std::int64_t Elevation::above_water(CellId cell) const {
    const std::int64_t ground = elevation(cell);
    const Evolved& evolved = this->evolved();
    if (evolved.rivers.standing(cell, ground)) {
        return 0;
    }
    const std::int64_t water = evolved.landform.grid().sample(
        evolved.rivers.levels(), cell_at(evolved.meander(cell_center(cell))));
    return std::max<std::int64_t>(ground - water, 0);
}

std::int64_t Elevation::worn(CellId cell, std::int64_t generated, const Landform& landform,
                             const Meander& meander) {
    if (generated <= 0) {
        return generated;
    }
    // The change fades out towards the shore, and low ground is never worn below a quarter of
    // its height, so coasts stay where generated and meet the sea without a step.
    // Sampled where the meanders shift the cell to, so valleys bend with the rivers in them.
    const std::int64_t change =
        (landform.change(cell_at(meander(cell_center(cell)))) * ramp(generated, coast_blend)) >>
        fixed_fraction_bits;
    return std::max(generated + change, (generated + 3) / 4);
}

template <int Bits>
std::int64_t Elevation::sea_floor(std::int64_t below) {
    constexpr std::int64_t one = fixed_unit<Bits>;
    constexpr std::int64_t break_depth = shelf_break / shelf_run;
    constexpr std::int64_t break_below = (shelf_break * one) / shore_metres;
    if (below >= break_below) {
        // Reaching the ocean floor where it always has.
        return -(break_depth * one) -
               ((ocean_depth - break_depth) *
                ramp<Bits>(below - break_below, depth_width<Bits> - break_below));
    }
    const std::int64_t out = below * shore_metres;
    // The cube root with 16 fractional bits, of metres with 48: inside the break, below 2^61.
    const auto root =
        static_cast<std::int64_t>(cube_root(static_cast<std::uint64_t>(out) << (48 - Bits)));
    const std::int64_t shoreface = (dean_scale * ((root * root) >> 16)) >> (32 - Bits);
    return -std::max(shoreface, out / shelf_run);
}

template std::int64_t Elevation::sea_floor<fixed_fraction_bits>(std::int64_t below);
template std::int64_t Elevation::sea_floor<fine_fraction_bits>(std::int64_t below);

std::int64_t Elevation::generated(CellId cell) const {
    // Generated finer than it's kept, so the noise's steps stay well below a centimetre.
    constexpr int finer = fine_fraction_bits - fixed_fraction_bits;
    return (generated<fine_fraction_bits>(cell) + (std::int64_t{1} << (finer - 1))) >> finer;
}

template <int Bits>
std::int64_t Elevation::generated(CellId cell) const {
    constexpr std::int64_t one = fixed_unit<Bits>;
    const std::int64_t continent = continents_.height<Bits>(cell);
    const std::int64_t height = archipelago_.raise<Bits>(cell, continent);
    if (height <= 0) {
        return sea_floor<Bits>(-height);
    }
    // Islands keep to lowland hills; only continents roll and carry mountain belts.
    const std::int64_t relief =
        (relief_height * ((ramp<Bits>(continent, inland_width<Bits>) *
                           fbm(relief_seed_, noise_point<Bits>(cell_center(cell), relief_frequency),
                               relief_octaves)) >>
                          Bits));
    const std::int64_t lowland =
        (((lowland_height * one) + relief) * ramp<Bits>(height, lowland_width<Bits>)) >> Bits;
    const std::int64_t strength = mountain_strength<Bits>(cell, continent);
    if (strength == 0) {
        return lowland;
    }
    const std::int64_t ridge =
        ridges(ridge_seed_, noise_point<Bits>(cell_center(cell), ridge_frequency), ridge_octaves,
               erosion<Bits>);
    return lowland + (mountain_height * ((strength * ridge) >> Bits));
}

} // namespace hog::planet
