#include <hog/planet/climate.hpp>
#include <hog/planet/projection.hpp>

namespace hog::planet {

namespace {

constexpr std::int64_t beach_temperature = 0;
// High ground colder than this through the year keeps its snow as glaciers.
constexpr std::int64_t glacier_temperature = -4 * fixed_one;
// The water table sits at this share of the height above water in a saturated climate, rising to
// the full height and beyond in a dry one.
constexpr std::int64_t wet_table_share = fixed_one / 5;
constexpr std::int64_t dry_table_share = fixed_one * 6 / 5;

[[nodiscard]] std::int64_t multiply(std::int64_t a, std::int64_t b) {
    return (a * b) >> fixed_fraction_bits;
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
    if (const Rainfall* rainfall = terrain_.rainfall()) {
        return wetness(rainfall->at(cell), temperature(cell));
    }
    return moisture_.at(cell, terrain_.continents().height(cell));
}

std::int64_t Climate::water_table(CellId cell) const {
    return water_table(cell, moisture(cell));
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
    const Rainfall* rainfall = terrain_.rainfall();
    surface.moisture = rainfall != nullptr ? wetness(rainfall->at(cell), surface.temperature)
                                           : moisture_.at(cell, terrain_.continents().height(cell));
    surface.water_table = water_table(cell, surface.moisture);
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
