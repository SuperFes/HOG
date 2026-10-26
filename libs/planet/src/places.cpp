#include <hog/core/assert.hpp>
#include <hog/planet/neighbors.hpp>
#include <hog/planet/places.hpp>
#include <hog/planet/projection.hpp>
#include <hog/planet/tile.hpp>

#include <algorithm>
#include <cstddef>
#include <ranges>
#include <utility>
#include <vector>

namespace hog::planet {

std::string_view to_string(Zone zone) noexcept {
    switch (zone) {
    case Zone::Polar:
        return "polar";
    case Zone::Cold:
        return "cold";
    case Zone::Temperate:
        return "temperate";
    case Zone::Tropical:
        return "tropical";
    case Zone::Arid:
        return "arid";
    }
    return "unknown";
}

Zone zone_of(const CellState& state) noexcept {
    return zone_of(state.temperature, state.moisture);
}

bool habitable(const CellState& state) noexcept {
    switch (state.biome) {
    case Biome::Ocean:
    case Biome::Lake:
    case Biome::Ice:
    case Biome::Alpine:
        return false;
    default:
        break;
    }
    const Zone zone = zone_of(state);
    return zone != Zone::Polar && zone != Zone::Arid;
}

bool coast(const Elevation& terrain, CellId cell) {
    return terrain.is_land(cell) && std::ranges::any_of(edge_neighbors(cell), [&](CellId next) {
               return !terrain.is_land(next);
           });
}

std::optional<CellId> nearest_cell(DVec3 from, int level, const std::function<bool(CellId)>& fits) {
    HOG_ASSERT(level >= 0 && level <= max_tile_depth, "too fine a level to search");
    const DVec3 toward = glm::normalize(from);
    std::vector<std::pair<double, CellId>> cells;
    cells.reserve(static_cast<std::size_t>(face_count) << (2 * level));
    for (int face = 0; face < face_count; ++face) {
        for (const CellId cell :
             sample_tile(CellId::from_face(face), level, [](CellId cell) { return cell; })) {
            // Nearest has the greatest cosine.
            cells.emplace_back(-glm::dot(cell_center(cell), toward), cell);
        }
    }
    std::ranges::sort(cells);
    for (const CellId cell : cells | std::views::values) {
        if (fits(cell)) {
            return cell;
        }
    }
    return std::nullopt;
}

} // namespace hog::planet
