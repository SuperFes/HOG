#include <hog/planet/climate.hpp>
#include <hog/planet/projection.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>

namespace hog::planet {

namespace {

constexpr std::int64_t beach_temperature = 0;
// High ground colder than this through the year keeps its snow as glaciers.
constexpr std::int64_t glacier_temperature = -4 * fixed_one;
// The water table sits at this share of the height above water in a saturated climate, rising to
// the full height and beyond in a dry one.
constexpr std::int64_t wet_table_share = fixed_one / 5;
constexpr std::int64_t dry_table_share = fixed_one * 6 / 5;

// Roots reach the water table this many metres down, so plants beside rivers and lakes draw on
// it however dry the climate.
constexpr std::int64_t rooting_depth = 10;
// The sun on the steepest slopes is held to this share of level ground's and this many times it.
constexpr double least_sunlit = 0.25;
constexpr double most_sunlit = 2.0;

// Climates this far from a boundary between biomes suit only their own.
constexpr std::int64_t blending_temperature = 2 * fixed_one;
constexpr std::int64_t blending_moisture = fixed_one / 20;
constexpr int blending_steps = 8;

[[nodiscard]] std::int64_t multiply(std::int64_t a, std::int64_t b) {
    return (a * b) >> fixed_fraction_bits;
}

} // namespace

BiomeBlend blend_biome(std::int64_t temperature, std::int64_t moisture) noexcept {
    const Biome own = classify_biome(temperature, moisture);
    BiomeBlend blend{.biome = own, .other = own, .share = 0};
    // Wetter, drier, warmer and colder, by the whole way out in fixed point.
    constexpr std::array<std::array<std::int64_t, 2>, 4> ways{{{0, 1}, {0, -1}, {1, 0}, {-1, 0}}};
    const auto at = [&](std::size_t way, std::int64_t out) {
        return classify_biome(temperature + (ways.at(way)[0] * multiply(blending_temperature, out)),
                              moisture + (ways.at(way)[1] * multiply(blending_moisture, out)));
    };
    for (int step = 1; step <= blending_steps; ++step) {
        for (std::size_t way = 0; way < ways.size(); ++way) {
            const Biome near = at(way, fixed_one * step / blending_steps);
            if (near == own) {
                continue;
            }
            // Where between the last step that stayed and this one the boundary lies.
            std::int64_t stayed = fixed_one * (step - 1) / blending_steps;
            std::int64_t crossed = fixed_one * step / blending_steps;
            while (crossed - stayed > 1) {
                const std::int64_t middle = (stayed + crossed) / 2;
                (at(way, middle) == own ? stayed : crossed) = middle;
            }
            blend.other = near;
            blend.share = (fixed_one - crossed) / 2;
            return blend;
        }
    }
    return blend;
}

double sunlit(DVec3 at, DVec3 uphill) {
    const double across = std::sqrt(std::max(0.0, 1.0 - (at.z * at.z)));
    const DVec3 north = DVec3{0.0, 0.0, 1.0} - (at * at.z);
    const double north_length = glm::length(north);
    const DVec3 equatorward =
        north_length > 0.0 ? north * ((at.z < 0.0 ? 1.0 : -1.0) / north_length) : DVec3{0.0};
    // At noon at the equinoxes, high as the latitude leaves it, towards the equator.
    const DVec3 sun = (at * across) + (equatorward * std::abs(at.z));
    const DVec3 facing = glm::normalize(at - uphill);
    const double level = std::max(across, least_sunlit);
    return std::clamp(glm::dot(facing, sun) / level, least_sunlit, most_sunlit);
}

std::int64_t plant_wetness(std::int64_t rain, std::int64_t temperature, double sunlit,
                           std::int64_t above_water) {
    // Rain against what more or less sun evaporates is as wet as less or more rain against
    // what level ground does.
    const std::int64_t wet = wetness(std::llround(static_cast<double>(rain) / sunlit), temperature);
    const std::int64_t reach =
        std::clamp(fixed_one - (above_water / rooting_depth), std::int64_t{0}, fixed_one);
    return wet + multiply(fixed_one - wet, multiply(reach, reach));
}

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
    case Biome::Lake:
        return "lake";
    }
    return "unknown";
}

Climate::Climate(const PlanetConfig& config)
    : terrain_{config}, temperature_{config.seed}, moisture_{config.seed} {}

std::int64_t Climate::temperature(CellId cell) const {
    return temperature(cell, terrain_.elevation(cell));
}

std::int64_t Climate::temperature(CellId cell, std::int64_t elevation) const {
    return temperature_.at(cell_center(cell), elevation);
}

std::int64_t Climate::moisture(CellId cell) const {
    if (terrain_.rainfall() == nullptr) {
        return moisture_.at(cell, terrain_.continents().height(cell));
    }
    const std::int64_t warmth = temperature(cell);
    return moisture(cell, warmth, climatic(cell, warmth));
}

std::int64_t Climate::climatic(CellId cell, std::int64_t temperature) const {
    if (const Rainfall* rainfall = terrain_.rainfall()) {
        return wetness(rainfall->at(cell), temperature);
    }
    return moisture_.at(cell, terrain_.continents().height(cell));
}

std::int64_t Climate::moisture(CellId cell, std::int64_t temperature, std::int64_t climatic) const {
    const Rainfall* rainfall = terrain_.rainfall();
    if (rainfall == nullptr) {
        return climatic;
    }
    return plant_wetness(rainfall->at(cell), temperature,
                         sunlit(cell_center(cell), terrain_.landform().uphill(cell)),
                         terrain_.above_water(cell));
}

std::int64_t Climate::water_table(CellId cell) const {
    if (terrain_.rainfall() == nullptr) {
        return water_table(cell, moisture(cell));
    }
    return water_table(cell, climatic(cell, temperature(cell)));
}

std::int64_t Climate::water_table(CellId cell, std::int64_t moisture) const {
    const std::int64_t share =
        dry_table_share - multiply(dry_table_share - wet_table_share, moisture);
    return multiply(terrain_.above_water(cell), share);
}

Biome Climate::biome(CellId cell) const {
    // Water needs no temperature.
    if (!terrain_.is_land(cell)) {
        return Biome::Ocean;
    }
    if (terrain_.lake(cell)) {
        return Biome::Lake;
    }
    return surface(cell, terrain_.elevation(cell)).biome;
}

Climate::Surface Climate::surface(CellId cell, std::int64_t elevation) const {
    Surface surface{.biome = Biome::Ocean,
                    .temperature = temperature(cell, elevation),
                    .moisture = fixed_one,
                    .water_table = 0};
    if (!terrain_.is_land(cell)) {
        return surface;
    }
    if (terrain_.lake(cell)) {
        surface.biome = Biome::Lake;
        return surface;
    }
    const std::int64_t wet = climatic(cell, surface.temperature);
    surface.moisture = moisture(cell, surface.temperature, wet);
    surface.water_table = water_table(cell, wet);
    const Biome climate = classify_biome(surface.temperature, surface.moisture);
    surface.biome = climate;
    if (climate == Biome::Ice) {
        return surface;
    }
    if (elevation > alpine_height * fixed_one) {
        surface.biome = surface.temperature < glacier_temperature ? Biome::Ice : Biome::Alpine;
    } else if (elevation < beach_height * fixed_one && surface.temperature > beach_temperature) {
        surface.biome = Biome::Beach;
    } else if (climate != Biome::Desert && surface.water_table < wetland_depth * fixed_one) {
        surface.biome = Biome::Wetland;
    }
    return surface;
}

} // namespace hog::planet
