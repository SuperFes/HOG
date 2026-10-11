#include <hog/core/assert.hpp>
#include <hog/planet/neighbors.hpp>
#include <hog/planet/projection.hpp>
#include <hog/server/spatial_index.hpp>

#include <algorithm>
#include <cmath>
#include <deque>
#include <numbers>
#include <unordered_set>

namespace hog::server {

using planet::CellId;

int level_for(double cell_size, double planet_radius) {
    HOG_ASSERT(cell_size > 0.0 && planet_radius > 0.0, "sizes must be positive");
    // A face spans a quarter of a great circle, halved at each level.
    double across = std::numbers::pi / 2.0 * planet_radius;
    int level = 0;
    while (across > cell_size && level < planet::max_level) {
        across /= 2.0;
        ++level;
    }
    return level;
}

SpatialIndex::SpatialIndex(int level) : level_(level) {
    HOG_ASSERT(level >= 0 && level <= planet::max_level, "level out of range");
}

CellId SpatialIndex::update(EntityId id, WorldPosition position) {
    const CellId cell = planet::cell_at(position, level_);
    if (const auto found = slots_.find(id); found != slots_.end()) {
        if (found->second.cell == cell) {
            cells_[cell.raw()][found->second.index].position = position;
            return cell;
        }
        remove(id);
    }
    std::vector<Entry>& entries = cells_[cell.raw()];
    slots_.emplace(id, Slot{.cell = cell, .index = static_cast<std::uint32_t>(entries.size())});
    entries.push_back({.id = id, .position = position});
    return cell;
}

void SpatialIndex::remove(EntityId id) {
    const auto found = slots_.find(id);
    if (found == slots_.end()) {
        return;
    }
    const auto [cell, index] = found->second;
    slots_.erase(found);
    std::vector<Entry>& entries = cells_.at(cell.raw());
    if (index + 1 != entries.size()) {
        entries[index] = entries.back();
        slots_.at(entries[index].id).index = index;
    }
    entries.pop_back();
    if (entries.empty()) {
        cells_.erase(cell.raw());
    }
}

std::optional<CellId> SpatialIndex::cell_of(EntityId id) const {
    if (const auto found = slots_.find(id); found != slots_.end()) {
        return found->second.cell;
    }
    return std::nullopt;
}

std::span<const SpatialIndex::Entry> SpatialIndex::in_cell(CellId cell) const {
    HOG_ASSERT(cell.level() == level_, "cell is not at the index's level");
    if (const auto found = cells_.find(cell.raw()); found != cells_.end()) {
        return found->second;
    }
    return {};
}

std::vector<CellId> SpatialIndex::cells_near(WorldPosition center, double radius) const {
    const double from_core = glm::length(center.metres());
    HOG_ASSERT(from_core > 0.0, "no cell lies under the planet's centre");
    const DVec3 up = center.metres() / from_core;

    // Two points at least `lowest` from the centre and within `radius` of each other are at most
    // this angle apart, seen from the centre.
    const double lowest = from_core - radius;
    const double reach =
        radius >= 2.0 * lowest ? std::numbers::pi : 2.0 * std::asin(radius / (2.0 * lowest));

    const auto angle = [](DVec3 a, DVec3 b) {
        return std::acos(std::clamp(glm::dot(a, b), -1.0, 1.0));
    };
    const CellId start = planet::cell_at(up, level_);
    std::vector<CellId> near;
    std::deque<CellId> frontier{start};
    std::unordered_set<std::uint64_t> seen{start.raw()};
    while (!frontier.empty()) {
        const CellId cell = frontier.front();
        frontier.pop_front();
        const DVec3 middle = planet::cell_center(cell);
        double spread = 0.0;
        for (const DVec3 corner : planet::cell_vertices(cell)) {
            spread = std::max(spread, angle(middle, glm::normalize(corner)));
        }
        if (cell != start && angle(up, middle) - spread > reach) {
            continue;
        }
        near.push_back(cell);
        const planet::VertexNeighbors neighbors = planet::vertex_neighbors(cell);
        for (const CellId neighbor : neighbors.cells()) {
            if (seen.insert(neighbor.raw()).second) {
                frontier.push_back(neighbor);
            }
        }
    }
    return near;
}

} // namespace hog::server
