#pragma once

#include <hog/core/result.hpp>
#include <hog/core/time.hpp>
#include <hog/planet/caves.hpp>
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
    std::int64_t temperature = 0; // fixed-point mean annual °C, at the generated surface
    std::int64_t moisture = 0;    // fixed-point, 0 (arid) to 1 (saturated)
    std::int64_t water_table = 0; // fixed-point metres down to it, from the generated surface
    std::int64_t excavated = 0;   // fixed-point metres dug out below the generated surface
    std::vector<Hollow> hollows;  // dug out below the ground left, shallowest first, never touching

    friend bool operator==(const CellState&, const CellState&) = default;
};

// Open space under a cell: a natural cave, or else dug out by someone.
struct Opening {
    std::optional<CaveHit> cave;

    friend bool operator==(const Opening&, const Opening&) = default;
};

// The planet as its seed plus every change made to it. A change to a cell also changes everything
// inside it; a cell's changes apply in layer order, then by game time, then in the order made.
class World {
public:
    explicit World(const PlanetConfig& config);

    // The caves point into the world's own geology.
    World(const World&) = delete;
    World(World&&) = delete;
    World& operator=(const World&) = delete;
    World& operator=(World&&) = delete;
    ~World() = default;

    [[nodiscard]] const Geology& geology() const noexcept { return geology_; }
    [[nodiscard]] const Caves& caves() const noexcept { return caves_; }

    // Fails, changing nothing, on an invalid cell, layer or payload.
    Result<void> apply(const Change& change);

    // Applies every change or, if any is invalid, none.
    Result<void> replay(std::span<const Change> changes);

    // Removes the matching changes, as if they were never made; returns how many.
    std::size_t roll_back(const ChangeFilter& filter);

    [[nodiscard]] CellState state(CellId cell) const;

    // The ground's fixed-point metres above sea level with its changes applied, as state has it,
    // without working out the rest of the state.
    [[nodiscard]] std::int64_t elevation(CellId cell) const;
    // The same from the cell's generated elevation, where that's known already.
    [[nodiscard]] std::int64_t elevation(CellId cell, std::int64_t generated) const;

    // What rock a cell's ground shows, as state has it; ground dug into is bare.
    [[nodiscard]] Exposure exposure(CellId cell, const CellState& state) const;

    // What is open at a fixed-point depth in metres below a cell's generated surface: dug out,
    // whether from the surface or as a hollow, or else a cave.
    [[nodiscard]] std::optional<Opening> opening_at(CellId cell, std::int64_t depth) const;

    // The matching changes, in the order they were made.
    [[nodiscard]] std::vector<Change> changes(const ChangeFilter& filter = {}) const;

    [[nodiscard]] std::size_t size() const noexcept { return count_; }

private:
    struct Entry {
        std::uint64_t sequence = 0;
        Change change;
    };

    // The changes to a cell and the cells it lies in, in the order they apply.
    [[nodiscard]] std::vector<const Entry*> applying(CellId cell) const;

    Geology geology_;
    Caves caves_;
    std::unordered_map<std::uint64_t, std::vector<Entry>> cells_;
    std::uint64_t next_sequence_ = 0;
    std::size_t count_ = 0;
};

} // namespace hog::planet
