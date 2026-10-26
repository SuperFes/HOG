#include <hog/core/hash.hpp>
#include <hog/planet/geology.hpp>
#include <hog/planet/hotspots.hpp>
#include <hog/planet/projection.hpp>
#include <hog/planet/tile.hpp>

#include <algorithm>
#include <cmath>

namespace hog::planet {

namespace {

constexpr std::uint64_t thickness_salt = 0x7468'6963'6B00'0000ULL; // "thick"
constexpr std::uint64_t deposit_salt = 0x6465'706F'7369'7400ULL;   // "deposit"
constexpr std::uint64_t crust_salt = 0x6372'7573'7400'0000ULL;     // "crust"
constexpr std::uint64_t chamber_salt = 0x6368'616D'6265'7200ULL;   // "chamber"

constexpr int octaves = 3;
// Metres across a sedimentary province and the swells in layer thickness.
constexpr double thickness_wavelength = 8'000.0;

constexpr std::int64_t ocean_sand = 3 * fixed_one;
constexpr std::int64_t ocean_sediment = 200 * fixed_one;
constexpr std::int64_t sand_cover = 2 * fixed_one;
// Continent height where the shelf gives way to ocean floor, a third of the way down the slope to
// the abyss (about 1,300 m deep), wandering up to `shelf_wander` either way.
constexpr std::int64_t shelf_edge = -fixed_one / 20;
constexpr std::int64_t shelf_wander = fixed_one / 50;

// Earth's crust: thin under the ocean floor, thickening up the continental slope to its full
// depth a little inland, with roots under mountains, swelling by a tenth either way.
constexpr std::int64_t ocean_crust = 7'000 * fixed_one;
constexpr std::int64_t continent_crust = 35'000 * fixed_one;
constexpr std::int64_t mountain_root = 25'000 * fixed_one;
constexpr std::int64_t crust_slope_foot = -fixed_one * 3 / 20;
constexpr std::int64_t crust_slope_width = fixed_one / 5;
constexpr std::int64_t crust_swell = fixed_one / 10;
constexpr double crust_wavelength = 30'000.0;

// Chambers sit 3 to 6 km down.
constexpr std::int64_t shallowest_chamber = 3'000;
constexpr std::int64_t deepest_chamber = 6'000;
constexpr std::int64_t chamber_half_height = 500 * fixed_one;
// An arc volcano's chamber sits as deep under its foot and reaches this much of the way out to
// it; a shield's is this wide. Every chamber keeps `mantle_clearance` above the mantle.
constexpr double arc_chamber_reach = 0.4;
constexpr double shield_chamber_radius = 3'000.0;
constexpr std::int64_t mantle_clearance = 500 * fixed_one;

// °C: deep sea water, the base of the crust, and magma, with the mantle warming per kilometre.
// The sea floor cools from the surface water's warmth over the first `cold_water_depth`.
constexpr std::int64_t sea_floor_temperature = 4 * fixed_one;
constexpr std::int64_t mantle_temperature = 800 * fixed_one;
constexpr std::int64_t mantle_warming = 10;
constexpr std::int64_t magma_temperature = 1'100 * fixed_one;
constexpr std::int64_t cold_water_depth = 1'000 * fixed_one;
// Metres from a chamber over which the rock's warming towards magma falls to about a quarter,
// and beyond which it is gone.
constexpr double magma_reach = 1'000.0;
constexpr double magma_limit = 10'000.0;

// The surface below which a seam sits.
enum class Host : std::uint8_t {
    Cover,    // straight under the cover
    Sediment, // inside the sedimentary rock, which must be thick enough
    Ground,   // under the cover in whatever rock is there
    Bedrock,  // under the sedimentary rock
};

struct Seam {
    Host host;
    double wavelength;       // metres across a typical patch
    std::int64_t threshold;  // noise above which the seam appears
    std::int64_t thickest;   // fixed-point metres
    std::int64_t min_offset; // fixed-point metres below the host surface
    std::int64_t max_offset;
};

constexpr std::array<Seam, resource_count> seams{{
    {.host = Host::Cover,
     .wavelength = 600.0,
     .threshold = fixed_one / 10,
     .thickest = 3 * fixed_one,
     .min_offset = 0,
     .max_offset = 0},
    {.host = Host::Sediment,
     .wavelength = 4'000.0,
     .threshold = fixed_one / 4,
     .thickest = 3 * fixed_one,
     .min_offset = 10 * fixed_one,
     .max_offset = 150 * fixed_one},
    {.host = Host::Sediment,
     .wavelength = 5'000.0,
     .threshold = fixed_one / 5,
     .thickest = 15 * fixed_one,
     .min_offset = 20 * fixed_one,
     .max_offset = 200 * fixed_one},
    {.host = Host::Ground,
     .wavelength = 1'500.0,
     .threshold = fixed_one / 5,
     .thickest = 5 * fixed_one,
     .min_offset = 2 * fixed_one,
     .max_offset = 60 * fixed_one},
    {.host = Host::Bedrock,
     .wavelength = 1'200.0,
     .threshold = fixed_one * 3 / 10,
     .thickest = 3 * fixed_one,
     .min_offset = 0,
     .max_offset = 40 * fixed_one},
    {.host = Host::Bedrock,
     .wavelength = 1'500.0,
     .threshold = fixed_one * 2 / 5,
     .thickest = 2 * fixed_one,
     .min_offset = 0,
     .max_offset = 40 * fixed_one},
    {.host = Host::Bedrock,
     .wavelength = 1'000.0,
     .threshold = fixed_one * 9 / 20,
     .thickest = fixed_one / 2,
     .min_offset = 10 * fixed_one,
     .max_offset = 80 * fixed_one},
}};

// Noise over which a seam thickens from nothing to its thickest.
constexpr std::int64_t seam_ramp = fixed_one / 4;
// Clay lies on low ground near the coast; salt only where it is dry; gold where mountains rose.
constexpr std::int64_t clay_below = 60 * fixed_one;
constexpr std::int64_t clay_above = -200 * fixed_one;
constexpr std::int64_t salt_moisture = fixed_one * 7 / 20;
constexpr std::int64_t gold_strength = fixed_one / 4;

[[nodiscard]] std::int64_t ramp(std::int64_t value, std::int64_t width) {
    return std::clamp((value * fixed_one) / width, std::int64_t{0}, fixed_one);
}

[[nodiscard]] std::int64_t multiply(std::int64_t a, std::int64_t b) {
    return (a * b) >> fixed_fraction_bits;
}

// 0 to 1 over `width`, easing in and out.
[[nodiscard]] std::int64_t smooth_ramp(std::int64_t value, std::int64_t width) {
    const std::int64_t t = ramp(value, width);
    return multiply(multiply(t, t), (3 * fixed_one) - (2 * t));
}

// How far rock this many metres from a chamber's wall is warmed towards magma, from 0 to 1.
[[nodiscard]] double magma_warmth(double distance) {
    const auto falloff = [](double away) {
        const double fall = magma_reach / (magma_reach + away);
        return fall * fall;
    };
    const double edge = falloff(magma_limit);
    return std::max((falloff(distance) - edge) / (1.0 - edge), 0.0);
}

// Noise in [-1, 1] mapped to [0, 1].
[[nodiscard]] std::int64_t unit(std::int64_t noise) {
    return (noise + fixed_one) / 2;
}

[[nodiscard]] bool may_hold(Resource resource, const Strata& strata, Biome biome,
                            std::int64_t elevation, std::int64_t moisture, std::int64_t mountains) {
    switch (resource) {
    case Resource::Clay:
        return elevation < clay_below && elevation > clay_above && biome != Biome::Desert &&
               biome != Biome::Ice && biome != Biome::Alpine;
    case Resource::Coal:
        return strata.sediment_thickness > 0;
    case Resource::Salt:
        return moisture < salt_moisture;
    case Resource::Iron:
    case Resource::Copper:
        return true;
    case Resource::Tin:
        return strata.bedrock == Rock::Granite;
    case Resource::Gold:
        return strata.bedrock == Rock::Granite && mountains > gold_strength;
    }
    return false;
}

} // namespace

std::optional<Resource> resource_at(const std::array<Deposit, resource_count>& deposits,
                                    std::int64_t depth) noexcept {
    for (const Resource resource : rarest_first) {
        const Deposit& deposit = deposits.at(static_cast<std::size_t>(resource));
        if (!deposit.empty() && depth >= deposit.top && depth < deposit.top + deposit.thickness) {
            return resource;
        }
    }
    return std::nullopt;
}

std::string_view to_string(Resource resource) noexcept {
    switch (resource) {
    case Resource::Clay:
        return "clay";
    case Resource::Coal:
        return "coal";
    case Resource::Salt:
        return "salt";
    case Resource::Iron:
        return "iron";
    case Resource::Copper:
        return "copper";
    case Resource::Tin:
        return "tin";
    case Resource::Gold:
        return "gold";
    }
    return "unknown";
}

Geology::Geology(const PlanetConfig& config)
    : climate_{config}, seed_{config.seed}, planet_radius_{config.radius},
      thickness_seed_{hash_combine(config.seed, thickness_salt)},
      deposit_seed_{hash_combine(config.seed, deposit_salt)},
      crust_seed_{hash_combine(config.seed, crust_salt)} {
    const std::uint64_t placement = hash_combine(config.seed, chamber_salt);
    const auto below = [&](int kind, std::size_t index) {
        const auto depth = static_cast<std::int64_t>(
            hash_values(placement, kind, index) %
            static_cast<std::uint64_t>(deepest_chamber - shallowest_chamber + 1));
        return (shallowest_chamber + depth) * fixed_one;
    };
    const auto volcanoes = terrain().volcanoes().volcanoes();
    const auto shields = terrain().hotspots().shields();
    chambers_.reserve(volcanoes.size() + terrain().hotspots().hotspots().size());
    for (std::size_t index = 0; index < volcanoes.size(); ++index) {
        const auto& volcano = volcanoes[index];
        const std::int64_t foot = terrain().elevation(cell_at(volcano.center)) -
                                  std::llround(volcano.height * static_cast<double>(fixed_one));
        add_chamber(volcano.center, foot, below(3, index), arc_chamber_reach * volcano.radius,
                    false);
    }
    for (std::size_t index = 0; index < shields.size(); index += Hotspots::chain_length) {
        add_chamber(shields[index].center, 0, below(4, index), shield_chamber_radius, true);
    }
}

void Geology::add_chamber(DVec3 center, std::int64_t level, std::int64_t below, double radius,
                          bool basaltic) {
    const CellId cell = cell_at(center);
    const std::int64_t deepest =
        crust_thickness(cell) - mantle_clearance - chamber_half_height - terrain().elevation(cell);
    chambers_.push_back({.center = center,
                         .depth = std::min(below - level, deepest),
                         .radius = radius,
                         .half_height = chamber_half_height,
                         .basaltic = basaltic});
}

NoisePoint Geology::noise_at(CellId cell, double wavelength) const {
    return noise_point(cell_center(cell), planet_radius_ / wavelength);
}

bool Geology::continental(CellId cell) const {
    const std::int64_t wander =
        multiply(shelf_wander, fbm(hash_combine(thickness_seed_, 1),
                                   noise_at(cell, thickness_wavelength), octaves));
    return terrain().continents().height(cell) > shelf_edge + wander;
}

Strata Geology::strata(CellId cell) const {
    Strata strata = this->strata(cell, climate_.biome(cell));
    const DVec3 here = cell_center(cell);
    for (const Chamber& chamber : chambers_) {
        const double across = glm::length(here - chamber.center) * planet_radius_ / chamber.radius;
        if (across >= 1.0) {
            continue;
        }
        const auto half = static_cast<std::int64_t>(std::sqrt(1.0 - (across * across)) *
                                                    static_cast<double>(chamber.half_height));
        strata.chamber_top = terrain().elevation(cell) + chamber.depth - half;
        strata.chamber_thickness = 2 * half;
        break;
    }
    return strata;
}

std::int64_t Geology::crust_thickness(CellId cell) const {
    const std::int64_t continent = terrain().continents().height(cell);
    const std::int64_t thickness =
        ocean_crust +
        multiply(continent_crust - ocean_crust,
                 smooth_ramp(continent - crust_slope_foot, crust_slope_width)) +
        multiply(mountain_root, terrain().mountain_strength(cell));
    const std::int64_t swell = fbm(crust_seed_, noise_at(cell, crust_wavelength), octaves);
    return thickness + multiply(thickness, multiply(crust_swell, swell));
}

Strata Geology::strata(CellId cell, Biome biome) const {
    const bool sea = biome == Biome::Ocean;
    if (sea && !continental(cell)) {
        return {.cover = Rock::Sand,
                .cover_thickness = ocean_sand,
                .sediment = Rock::Shale,
                .sediment_thickness = ocean_sediment,
                .bedrock = Rock::Basalt,
                .mantle_depth = crust_thickness(cell)};
    }
    const std::int64_t swell = fbm(thickness_seed_, noise_at(cell, thickness_wavelength), octaves);
    const std::int64_t mountains = terrain().mountain_strength(cell);
    Strata strata{.mantle_depth = crust_thickness(cell)};
    switch (biome) {
    case Biome::Beach:
    case Biome::Desert:
        strata.cover = Rock::Sand;
        strata.cover_thickness = sand_cover;
        break;
    case Biome::Ocean:
        strata.cover = Rock::Sand;
        strata.cover_thickness = ocean_sand;
        break;
    case Biome::Ice:
    case Biome::Alpine:
        break;
    default:
        // One to two metres of soil.
        strata.cover_thickness = multiply(fixed_one + unit(swell), fixed_one - mountains);
    }
    // Land off the continents is volcanic, with nothing laid down on it yet.
    if (!sea && !terrain().continents().is_land(cell)) {
        strata.bedrock = Rock::Basalt;
        return strata;
    }
    Layering layering = terrain().layering(cell);
    strata.beds = &terrain().beds();
    if (!layering.column.continental) {
        // The shelf beyond the shore, still on the continent's rock.
        layering.column = strata.beds->column(cell, {.continental = true, .mountains = mountains});
    }
    strata.column = layering.column;
    strata.surface = terrain().elevation(cell) - layering.risen;
    const std::int64_t under = strata.surface - strata.cover_thickness;
    strata.sediment = strata.beds->rock_at(layering.column, under);
    strata.sediment_thickness = std::max<std::int64_t>(under - layering.column.basement, 0);
    return strata;
}

std::int64_t Geology::rock_temperature(CellId cell, std::int64_t depth) const {
    const std::int64_t elevation = terrain().elevation(cell);
    std::int64_t surface = climate_.temperature(cell);
    if (elevation < 0) {
        surface += multiply(sea_floor_temperature - surface, ramp(-elevation, cold_water_depth));
    }
    const auto deepest = static_cast<std::int64_t>(planet_radius_) * fixed_one;
    depth = std::clamp(depth, std::int64_t{0}, deepest);
    const std::int64_t mantle = crust_thickness(cell);
    std::int64_t heat = depth < mantle
                            ? surface + ((mantle_temperature - surface) * depth / mantle)
                            : mantle_temperature + ((depth - mantle) * mantle_warming / 1'000);

    const DVec3 here = cell_center(cell);
    const double below_sea = fixed_to_double(depth - elevation);
    double warmth = 0.0;
    for (const Chamber& chamber : chambers_) {
        const double across = glm::length(here - chamber.center) * planet_radius_;
        if (across >= chamber.radius + magma_limit) {
            continue;
        }
        const double down = below_sea - fixed_to_double(chamber.depth);
        const double half = fixed_to_double(chamber.half_height);
        // How far out the point is in the chamber's own shape, and so its distance from the
        // chamber's wall along the line from its middle.
        const double reach = std::sqrt(((across * across) / (chamber.radius * chamber.radius)) +
                                       ((down * down) / (half * half)));
        const double distance =
            reach <= 1.0 ? 0.0
                         : std::sqrt((across * across) + (down * down)) * (1.0 - (1.0 / reach));
        warmth = std::max(warmth, magma_warmth(distance));
    }
    if (heat < magma_temperature) {
        heat += static_cast<std::int64_t>(
            std::llround(static_cast<double>(magma_temperature - heat) * warmth));
    }
    return heat;
}

std::array<Deposit, resource_count> Geology::deposits(CellId cell) const {
    std::array<Deposit, resource_count> found{};
    const Biome biome = climate_.biome(cell);
    if (biome == Biome::Ocean && !continental(cell)) {
        return found;
    }
    const Strata strata = this->strata(cell, biome);
    const std::int64_t elevation = terrain().elevation(cell);
    const std::int64_t moisture = climate_.moisture(cell);
    const std::int64_t mountains = terrain().mountain_strength(cell);
    for (int index = 0; index < resource_count; ++index) {
        const auto resource = static_cast<Resource>(index);
        const Seam& seam = seams.at(static_cast<std::size_t>(index));
        if (!may_hold(resource, strata, biome, elevation, moisture, mountains)) {
            continue;
        }
        const std::uint64_t seed = hash_values(deposit_seed_, index);
        std::int64_t thickness = multiply(
            seam.thickest,
            ramp(fbm(seed, noise_at(cell, seam.wavelength), octaves) - seam.threshold, seam_ramp));
        // The depth varies more gently than the thickness.
        const std::int64_t offset =
            seam.min_offset + multiply(seam.max_offset - seam.min_offset,
                                       unit(fbm(hash_combine(seed, 1),
                                                noise_at(cell, seam.wavelength * 4.0), octaves)));
        std::int64_t top = offset;
        switch (seam.host) {
        case Host::Cover:
        case Host::Ground:
            top += strata.cover_thickness;
            break;
        case Host::Sediment:
            top += strata.cover_thickness;
            thickness = std::min(thickness, strata.sediment_thickness - offset);
            if (resource == Resource::Coal) {
                // Coal measures lie in shale and sandstone, a seam within one bed.
                const auto bed = strata.bed_at(top);
                if (!bed || (bed->rock != Rock::Shale && bed->rock != Rock::Sandstone)) {
                    continue;
                }
                thickness = std::min(thickness, strata.surface - bed->base - top);
            }
            break;
        case Host::Bedrock:
            top += strata.bedrock_depth();
            break;
        }
        if (thickness > 0) {
            found.at(static_cast<std::size_t>(index)) = {.top = top, .thickness = thickness};
        }
    }
    return found;
}

double Geology::volume(CellId cell, const Deposit& deposit) const {
    return fixed_to_double(deposit.thickness) * cell_area(cell) * planet_radius_ * planet_radius_;
}

} // namespace hog::planet
