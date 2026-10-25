#include <hog/core/hash.hpp>
#include <hog/planet/bake.hpp>
#include <hog/planet/elevation.hpp>
#include <hog/planet/moisture.hpp>
#include <hog/planet/noise.hpp>
#include <hog/planet/projection.hpp>
#include <hog/planet/source_hash.hpp>
#include <hog/planet/tile.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <mutex>
#include <tuple>
#include <vector>

namespace hog::planet {

namespace {

constexpr std::uint64_t ridge_salt = 0x7269'6467'6573'0000ULL;  // "ridges"
constexpr std::uint64_t relief_salt = 0x7265'6C69'6566'0000ULL; // "relief"
constexpr std::uint64_t hill_salt = 0x6869'6C6C'7300'0000ULL;   // "hills"

// Continent heights (fixed point) over which each effect ramps from nothing to full.
template <int Bits>
constexpr std::int64_t depth_width = fixed_unit<Bits> * 3 / 20;
template <int Bits>
constexpr std::int64_t lowland_width = fixed_unit<Bits> / 5;
template <int Bits>
constexpr std::int64_t inland_width = fixed_unit<Bits> / 10;

// Continental lowlands roll by some tens of metres over a dozen kilometres or so, so no plain is
// dead flat and water always finds a way down.
constexpr std::int64_t relief_height = 80;
constexpr double relief_frequency = 8.0;
constexpr int relief_octaves = 4;

// Ridges lie this far apart, and run this many times longer along a range than across it.
constexpr double ridge_wavelength = 12'000.0; // metres
constexpr std::int64_t ridge_stretch = 2;
constexpr int ridge_octaves = 6;
// The ocean floor's hills, lying along the ridges the floor was made at.
constexpr double hill_wavelength = 4'000.0; // metres
constexpr int hill_octaves = 3;
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

// Where HOG_BAKES says landforms are kept, or nowhere.
std::filesystem::path bakes_named() {
#ifdef _MSC_VER
#pragma warning(suppress : 4996)
#endif
    // NOLINTNEXTLINE(concurrency-mt-unsafe): nothing sets the environment once running
    const char* const named = std::getenv("HOG_BAKES");
    return named != nullptr ? std::filesystem::path{named} : std::filesystem::path{};
}

// 0 at or below zero, rising linearly to 1 at `width`.
template <int Bits = fixed_fraction_bits>
[[nodiscard]] std::int64_t ramp(std::int64_t value, std::int64_t width) {
    return std::clamp((value * fixed_unit<Bits>) / width, std::int64_t{0}, fixed_unit<Bits>);
}

} // namespace

Elevation::Elevation(const PlanetConfig& config)
    : continents_{config.seed, config.ocean_fraction,
                  std::make_shared<const Plates>(config.seed,
                                                 LevelGrid{Plates::level, config.radius},
                                                 config.radius, config.plates)},
      archipelago_{config.seed, config.radius, continents_},
      volcanoes_{config.seed, config.radius, continents_},
      hotspots_{config.seed, config.radius, continents_, archipelago_.center()},
      ridge_seed_{hash_combine(config.seed, ridge_salt)},
      ridge_frequency_{config.radius / ridge_wavelength},
      hill_seed_{hash_combine(config.seed, hill_salt)},
      hill_frequency_{config.radius / hill_wavelength},
      relief_seed_{hash_combine(config.seed, relief_salt)},
      evolving_{std::make_shared<Evolving>(config)} {}

Elevation::Evolved::Evolved(Landform worn, std::span<const std::int64_t> ground,
                            const PlanetConfig& config)
    : landform{std::move(worn)}, meander{config.seed, landform.grid()},
      rivers{landform, ground, meander, config.radius, config.river_scale} {}

const Elevation::Evolved& Elevation::evolved() const {
    std::call_once(evolving_->once, [this] { evolving_->evolved = evolve(evolving_->config); });
    return *evolving_->evolved;
}

std::shared_ptr<const Elevation::Evolved> Elevation::evolve(const PlanetConfig& config) const {
    // Evolving takes a while, so it's shared while any Elevation of the same planet lives.
    using Key = std::tuple<std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t, int>;
    static std::mutex mutex;
    static std::map<Key, std::weak_ptr<const Evolved>> evolved;
    const Key key{config.seed, std::bit_cast<std::uint64_t>(config.radius),
                  std::bit_cast<std::uint64_t>(config.ocean_fraction),
                  std::bit_cast<std::uint64_t>(config.river_scale), config.plates};
    const std::scoped_lock lock{mutex};
    if (auto shared = evolved[key].lock()) {
        return shared;
    }

    const int level = std::min(level_for(landform_cell, config.radius), landform_level);
    // Baked by this very code, so changing it evolves the world again.
    const BakeName bake{.world = hash_values(world_id(config), static_cast<std::uint64_t>(level),
                                             static_cast<std::uint64_t>(landform_steps)),
                        .code = planet_source_hash};
    const std::filesystem::path bakes = config.bakes.empty() ? bakes_named() : config.bakes;
    std::shared_ptr<const Evolved> shared;
    if (!bakes.empty()) {
        shared = unbake(read_bake(bakes, bake), level, config);
    }
    if (!shared) {
        Landform landform = wear(LevelGrid{level, config.radius}, config.seed);
        const Meander meander{config.seed, landform.grid()};
        std::vector<std::int64_t> ground(landform.grid().size());
        for (std::uint32_t index = 0; index < landform.grid().size(); ++index) {
            const CellId cell = landform.grid().cell(index);
            ground[index] = worn(cell, generated(cell), landform, meander);
        }
        if (!bakes.empty()) {
            std::ignore =
                write_bake(bakes, bake,
                           {.level = level,
                            .heights = {landform.heights().begin(), landform.heights().end()},
                            .change = {landform.change().begin(), landform.change().end()},
                            .drained = {landform.drained().begin(), landform.drained().end()},
                            .ground = ground});
        }
        shared = std::make_shared<const Evolved>(std::move(landform), ground, config);
    }
    evolved[key] = shared;
    return shared;
}

std::shared_ptr<const Elevation::Evolved> Elevation::unbake(std::optional<LandformBake> bake,
                                                            int level, const PlanetConfig& config) {
    if (!bake || bake->level != level) {
        return nullptr;
    }
    LevelGrid grid{level, config.radius};
    if (bake->heights.size() != grid.size()) {
        return nullptr;
    }
    return std::make_shared<const Evolved>(Landform{std::move(grid), std::move(bake->heights),
                                                    std::move(bake->change),
                                                    std::move(bake->drained)},
                                           bake->ground, config);
}

Landform Elevation::wear(LevelGrid grid, std::uint64_t seed) const {
    const auto fixed = [](double value) {
        return std::llround(value * static_cast<double>(fixed_one));
    };
    const Moisture moisture{seed};
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
    return Landform{std::move(grid), std::move(heights),
                    Erosion{.uplift = uplift, .erodibility = hardness, .runoff = runoff},
                    landform_steps};
}

std::int64_t Elevation::mountain_strength(CellId cell) const {
    // As strong as the generated ground has it.
    constexpr int finer = fine_fraction_bits - fixed_fraction_bits;
    const auto crust = continents_.crust<fine_fraction_bits>(cell);
    return mountain_strength<fine_fraction_bits>(crust.height, crust.mountains) >> finer;
}

template <int Bits>
std::int64_t Elevation::mountain_strength(std::int64_t height, std::int64_t mountains) {
    return (ramp<Bits>(height, inland_width<Bits>) * mountains) >> Bits;
}

std::int64_t Elevation::elevation(CellId cell) const {
    const std::int64_t generated = this->generated(cell);
    const Evolved& evolved = this->evolved();
    const std::int64_t ground = worn(cell, generated, evolved.landform, evolved.meander);
    // Rivers end at the sea, cutting nothing under it.
    return generated <= 0 ? ground : evolved.rivers.cut(cell, ground);
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
    // Told by the fine elevation, so ground right at sea level is the sea's.
    if (generated<fine_fraction_bits>(cell) > 0) {
        return true;
    }
    // Lakes build their rims up out of low ground.
    const std::int64_t ground = elevation(cell);
    if (ground > 0) {
        return true;
    }
    const auto water = evolved().rivers.standing(cell, std::min<std::int64_t>(ground, -1));
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
    if (generated <= 0 && !landform.near_lake(cell)) {
        return generated;
    }
    // Sampled where the meanders shift the cell to, so valleys bend with the rivers in them.
    const CellId shifted = cell_at(meander(cell_center(cell)));
    std::int64_t ground = generated;
    if (generated > 0) {
        // The change fades out towards the shore, and low ground is never worn below a quarter
        // of its height, so coasts stay where generated and meet the sea without a step.
        const std::int64_t change =
            (landform.change(shifted) * ramp(generated, coast_blend)) >> fixed_fraction_bits;
        ground = std::max(generated + change, (generated + 3) / 4);
    }
    // Lakes fill the landform's ground under their levels, and the cells round them stand above,
    // so the ground about them is the landform's, shading back into the finer ground beyond.
    const std::int64_t lakeside = landform.grid().sample(landform.lakeside(), shifted);
    if (lakeside == 0) {
        return ground;
    }
    const std::int64_t smooth = landform.grid().sample(landform.heights(), shifted);
    return ground + (((smooth - ground) * lakeside) >> fixed_fraction_bits);
}

template <int Bits>
std::int64_t Elevation::sea_floor(std::int64_t below, std::int64_t abyss) {
    constexpr std::int64_t one = fixed_unit<Bits>;
    constexpr std::int64_t break_depth = shelf_break / shelf_run;
    constexpr std::int64_t break_below = (shelf_break * one) / shore_metres;
    if (below >= break_below) {
        // Reaching the ocean floor where it always has; however much is built up under it, the
        // floor stays as deep as the shelf's edge.
        return -(break_depth * one) -
               (((std::max(abyss, break_depth * one) - (break_depth * one)) *
                 ramp<Bits>(below - break_below, depth_width<Bits> - break_below)) >>
                Bits);
    }
    const std::int64_t out = below * shore_metres;
    // The cube root with 16 fractional bits, of metres with 48: inside the break, below 2^61.
    const auto root =
        static_cast<std::int64_t>(cube_root(static_cast<std::uint64_t>(out) << (48 - Bits)));
    const std::int64_t shoreface = (dean_scale * ((root * root) >> 16)) >> (32 - Bits);
    return -std::max(shoreface, out / shelf_run);
}

template std::int64_t Elevation::sea_floor<fixed_fraction_bits>(std::int64_t below,
                                                                std::int64_t abyss);
template std::int64_t Elevation::sea_floor<fine_fraction_bits>(std::int64_t below,
                                                               std::int64_t abyss);

template <int Bits>
std::int64_t Elevation::ridge(std::uint64_t seed, double frequency, int octaves, CellId cell,
                              const std::array<std::int64_t, 3>& grain) {
    // Noise stretched along each axis, weighted by how far the range runs along it: stretching
    // along fixed axes bends nothing, however the range turns.
    const auto point = noise_point<Bits>(cell_center(cell), frequency);
    std::int64_t total = 0;
    std::int64_t weights = 0;
    for (std::size_t axis = 0; axis < 3; ++axis) {
        // Squared, so the axis nearest the grain leads and blending blurs the crests less.
        const std::int64_t weight = (grain.at(axis) * grain.at(axis)) >> fixed_fraction_bits;
        if (weight < fixed_one / 50) {
            continue;
        }
        auto stretched = point;
        std::array<std::int64_t*, 3> along{&stretched.x, &stretched.y, &stretched.z};
        *along.at(axis) /= ridge_stretch;
        total += weight * ridges(hash_values(seed, axis), stretched, octaves, erosion<Bits>);
        weights += weight;
    }
    if (weights == 0) {
        return ridges(seed, point, octaves, erosion<Bits>);
    }
    return total / weights;
}

std::int64_t Elevation::generated(CellId cell) const {
    // Generated finer than it's kept, so the noise's steps stay well below a centimetre.
    constexpr int finer = fine_fraction_bits - fixed_fraction_bits;
    return (generated<fine_fraction_bits>(cell) + (std::int64_t{1} << (finer - 1))) >> finer;
}

template <int Bits>
std::int64_t Elevation::generated(CellId cell) const {
    constexpr std::int64_t one = fixed_unit<Bits>;
    const auto crust = continents_.crust<Bits>(cell);
    const std::int64_t continent = crust.height;
    const std::int64_t height = archipelago_.raise<Bits>(cell, continent);
    const std::int64_t volcanoes = volcanoes_.lift<Bits>(cell);
    // Shields stand over whatever else is there, their lava burying it.
    const auto shields = hotspots_.surface<Bits>(cell);
    const auto built = [&](std::int64_t ground) {
        return shields ? std::max(ground, *shields) : ground;
    };
    if (height <= 0) {
        const std::int64_t hills =
            (abyssal_hill_height *
             (ridge<Bits>(hill_seed_, hill_frequency_, hill_octaves, cell, crust.grain) -
              (one / 2)));
        const std::int64_t abyss =
            (ocean_depth * one) - crust.floor - hills - hotspots_.swell<Bits>(cell);
        return built(sea_floor<Bits>(-height, abyss) + volcanoes);
    }
    // Islands keep to lowland hills; only continents roll and carry mountain belts.
    const std::int64_t relief =
        (relief_height * ((ramp<Bits>(continent, inland_width<Bits>) *
                           fbm(relief_seed_, noise_point<Bits>(cell_center(cell), relief_frequency),
                               relief_octaves)) >>
                          Bits));
    const std::int64_t lowland =
        ((((lowland_height * one) + relief) * ramp<Bits>(height, lowland_width<Bits>)) >> Bits) +
        archipelago_.volcano<Bits>(cell);
    const std::int64_t strength = mountain_strength<Bits>(continent, crust.mountains);
    if (strength == 0) {
        return built(lowland + volcanoes);
    }
    const std::int64_t ridge =
        Elevation::ridge<Bits>(ridge_seed_, ridge_frequency_, ridge_octaves, cell, crust.grain);
    return built(lowland + (mountain_height * ((strength * ridge) >> Bits)) + volcanoes);
}

} // namespace hog::planet
