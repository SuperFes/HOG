#pragma once

#include <hog/core/math.hpp>
#include <hog/planet/cell_id.hpp>
#include <hog/planet/climate.hpp>
#include <hog/planet/elevation.hpp>
#include <hog/planet/world.hpp>

#include <cstdint>
#include <functional>
#include <optional>
#include <string_view>

namespace hog::planet {

// Broad climate zones, as the biomes fall into them.
enum class Zone : std::uint8_t {
    Polar,
    Cold,
    Temperate,
    Tropical,
    Arid,
};

inline constexpr int zone_count = static_cast<int>(Zone::Arid) + 1;

[[nodiscard]] std::string_view to_string(Zone zone) noexcept;

// From fixed-point mean annual temperature (°C) and moisture (0 to 1), on the biomes' own lines.
[[nodiscard]] constexpr Zone zone_of(std::int64_t temperature, std::int64_t moisture) noexcept {
    switch (classify_biome(temperature, moisture)) {
    case Biome::Ice:
    case Biome::Tundra:
        return Zone::Polar;
    case Biome::BorealForest:
        return Zone::Cold;
    case Biome::Desert:
        return Zone::Arid;
    case Biome::Savanna:
    case Biome::TropicalRainforest:
        return Zone::Tropical;
    default:
        return Zone::Temperate;
    }
}

[[nodiscard]] Zone zone_of(const CellState& state) noexcept;

// Dry, open ground in a zone people can live off: not under water or ice, not bare peaks, nor
// polar or desert.
[[nodiscard]] bool habitable(const CellState& state) noexcept;

// Land with the sea beside it across an edge.
[[nodiscard]] bool coast(const Elevation& terrain, CellId cell);

// The cell at `level` nearest `from` where `fits` holds, or nothing where none does. Cells are
// tried nearest first, but any may be, so the level should be coarse: at 8 there are some 400,000.
[[nodiscard]] std::optional<CellId> nearest_cell(DVec3 from, int level,
                                                 const std::function<bool(CellId)>& fits);

} // namespace hog::planet
