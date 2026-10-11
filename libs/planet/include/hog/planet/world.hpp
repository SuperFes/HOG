#pragma once

#include <hog/core/result.hpp>
#include <hog/core/time.hpp>
#include <hog/planet/cell_id.hpp>
#include <hog/planet/change.hpp>
#include <hog/planet/geology.hpp>
#include <hog/planet/planet_config.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <unordered_map>
#include <vector>

namespace hog::planet {

// What a cell looks like once its changes are applied over the generated planet.
struct CellState {
    std::int64_t elevation = 0; // fixed-point metres above sea level
    Biome biome = Biome::Ocean;
    std::int64_t excavated = 0; // fixed-point metres dug out below the generated surface

    friend bool operator==(const CellState&, const CellState&) = default;
};

// Selects changes, for rolling back or listing. Every set field must match.
struct ChangeFilter {
    std::optional<AuthorId> author;
    std::optional<Layer> layer;
    // Game time from `from` up to, not including, `until`.
    GameTime from = GameTime::min();
    GameTime until = GameTime::max();

    [[nodiscard]] bool matches(const Change& change) const noexcept;
};

// The planet as its seed plus every change made to it. A change to a cell also changes everything
// inside it; a cell's changes apply in layer order, then by game time, then in the order made.
class World {
public:
    explicit World(const PlanetConfig& config);

    [[nodiscard]] const Geology& geology() const noexcept { return geology_; }

    // Fails, changing nothing, on an invalid cell, layer or payload.
    Result<void> apply(const Change& change);

    // Applies every change or, if any is invalid, none.
    Result<void> replay(std::span<const Change> changes);

    // Removes the matching changes, as if they were never made; returns how many.
    std::size_t roll_back(const ChangeFilter& filter);

    [[nodiscard]] CellState state(CellId cell) const;

    // The matching changes, in the order they were made.
    [[nodiscard]] std::vector<Change> changes(const ChangeFilter& filter = {}) const;

    [[nodiscard]] std::size_t size() const noexcept { return count_; }

private:
    struct Entry {
        std::uint64_t sequence = 0;
        Change change;
    };

    Geology geology_;
    std::unordered_map<std::uint64_t, std::vector<Entry>> cells_;
    std::uint64_t next_sequence_ = 0;
    std::size_t count_ = 0;
};

} // namespace hog::planet
