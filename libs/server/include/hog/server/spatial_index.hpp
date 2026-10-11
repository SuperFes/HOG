#pragma once

#include <hog/core/math.hpp>
#include <hog/planet/cell_id.hpp>

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <unordered_map>
#include <vector>

namespace hog::server {

// flecs entity ids, though the index itself knows nothing of the ECS.
using EntityId = std::uint64_t;

// The coarsest cube-sphere level whose cells are at most `cell_size` metres across on average.
[[nodiscard]] int level_for(double cell_size, double planet_radius);

// Entities bucketed by the cube-sphere cell under them, at one level. A cell is a column from the
// planet's centre outwards, so altitude never moves an entity between cells.
class SpatialIndex {
public:
    struct Entry {
        EntityId id = 0;
        WorldPosition position;
    };

    // Precondition: 0 <= level <= planet::max_level.
    explicit SpatialIndex(int level);

    [[nodiscard]] int level() const noexcept { return level_; }
    [[nodiscard]] std::size_t size() const noexcept { return slots_.size(); }

    // Places or moves an entity and returns its cell. Precondition: the position is not the
    // planet's centre.
    planet::CellId update(EntityId id, WorldPosition position);
    // Does nothing for an entity the index doesn't hold.
    void remove(EntityId id);

    [[nodiscard]] std::optional<planet::CellId> cell_of(EntityId id) const;
    // Precondition: the cell is at the index's level.
    [[nodiscard]] std::span<const Entry> in_cell(planet::CellId cell) const;

    // Calls visit(entry) for every entity within `radius` metres of `center`.
    template <typename F>
        requires std::invocable<F&, const Entry&>
    void for_each_near(WorldPosition center, double radius, F visit) const {
        const double squared = radius * radius;
        for (const planet::CellId cell : cells_near(center, radius)) {
            for (const Entry& entry : in_cell(cell)) {
                const DVec3 offset = entry.position - center;
                if (glm::dot(offset, offset) <= squared) {
                    visit(entry);
                }
            }
        }
    }

private:
    struct Slot {
        planet::CellId cell;
        std::uint32_t index = 0;
    };

    // Every cell at the index's level that could hold a point within `radius` of `center`.
    [[nodiscard]] std::vector<planet::CellId> cells_near(WorldPosition center, double radius) const;

    int level_;
    std::unordered_map<std::uint64_t, std::vector<Entry>> cells_;
    std::unordered_map<EntityId, Slot> slots_;
};

} // namespace hog::server
