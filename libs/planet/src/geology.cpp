#include <hog/core/hash.hpp>
#include <hog/planet/geology.hpp>
#include <hog/planet/projection.hpp>
#include <hog/planet/tile.hpp>

#include <algorithm>

namespace hog::planet {

namespace {

constexpr std::uint64_t facies_salt = 0x6661'6369'6573'0000ULL;    // "facies"
constexpr std::uint64_t thickness_salt = 0x7468'6963'6B00'0000ULL; // "thick"
constexpr std::uint64_t deposit_salt = 0x6465'706F'7369'7400ULL;   // "deposit"

constexpr int octaves = 3;
// Metres across a sedimentary province and the swells in layer thickness.
constexpr double facies_wavelength = 20'000.0;
constexpr double thickness_wavelength = 8'000.0;

constexpr std::int64_t ocean_sand = 3 * fixed_one;
constexpr std::int64_t ocean_sediment = 200 * fixed_one;
constexpr std::int64_t sand_cover = 2 * fixed_one;
constexpr std::int64_t max_sediment = 600 * fixed_one;
// Continent height where the shelf gives way to ocean floor, a third of the way down the slope to
// the abyss (about 1,300 m deep), wandering up to `shelf_wander` either way.
constexpr std::int64_t shelf_edge = -fixed_one / 20;
constexpr std::int64_t shelf_wander = fixed_one / 50;

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
        return strata.sediment == Rock::Shale || strata.sediment == Rock::Sandstone;
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

std::string_view to_string(Rock rock) noexcept {
    switch (rock) {
    case Rock::Soil:
        return "soil";
    case Rock::Sand:
        return "sand";
    case Rock::Sandstone:
        return "sandstone";
    case Rock::Shale:
        return "shale";
    case Rock::Limestone:
        return "limestone";
    case Rock::Granite:
        return "granite";
    case Rock::Basalt:
        return "basalt";
    }
    return "unknown";
}

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
    : climate_{config}, planet_radius_{config.radius},
      facies_seed_{hash_combine(config.seed, facies_salt)},
      thickness_seed_{hash_combine(config.seed, thickness_salt)},
      deposit_seed_{hash_combine(config.seed, deposit_salt)} {}

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
    return strata(cell, climate_.biome(cell));
}

Strata Geology::strata(CellId cell, Biome biome) const {
    const bool sea = biome == Biome::Ocean;
    if (sea && !continental(cell)) {
        return {.cover = Rock::Sand,
                .cover_thickness = ocean_sand,
                .sediment = Rock::Shale,
                .sediment_thickness = ocean_sediment,
                .bedrock = Rock::Basalt};
    }
    const std::int64_t swell = fbm(thickness_seed_, noise_at(cell, thickness_wavelength), octaves);
    // Mountains wear their cover and sedimentary rock away down to the granite.
    const std::int64_t mountains = terrain().mountain_strength(cell);
    const std::int64_t exposure = multiply(fixed_one - mountains, fixed_one - mountains);
    Strata strata{};
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
    // The starting islands are volcanic, with nothing laid down on them yet.
    if (!sea && terrain().continents().height(cell) <= 0) {
        strata.bedrock = Rock::Basalt;
        return strata;
    }
    const std::int64_t facies = fbm(facies_seed_, noise_at(cell, facies_wavelength), octaves);
    if (facies < -fixed_one / 4) {
        strata.sediment = Rock::Sandstone;
    } else if (facies > fixed_one / 4) {
        strata.sediment = Rock::Limestone;
    }
    // Thin to nothing in the deepest troughs of the swell, leaving granite outcrops.
    strata.sediment_thickness =
        multiply(multiply(max_sediment, exposure), ramp(swell + (fixed_one / 2), fixed_one));
    return strata;
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
