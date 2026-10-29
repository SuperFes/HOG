#pragma once

#include <hog/core/math.hpp>
#include <hog/planet/level_grid.hpp>

#include <cstdint>
#include <span>
#include <vector>

namespace hog::planet {

// The crust as its plates move over time on a coarse grid, each turning about its own pole: where
// plates part, new ocean floor wells up between them; where they meet, the denser crust dives
// under the other, or continents pile up against each other into ranges. Steps are as long as
// takes the plates a cell or two, so a small planet's crust doesn't spin round it.
class Tectonics {
public:
    struct Settings {
        int level = 7; // of the grid the crust lies on
        int plates = 10;
        // Of the crust, how much starts continental, gathered into one supercontinent.
        double continental = 0.3;
        // Cells the plates move each step, on average. Crust is carried from cell to cell, so a
        // plate turning less than half a cell a step never moves.
        double travel = 1.5;
        // Behind where ocean floor dives under a plate, magma rising from it thickens the crust
        // over so many cells by so many kilometres a step.
        int arc_reach = 3;
        float arc_growth = 0.6F;
        // Of the difference in thickness between neighbouring continental crust, the share that
        // spreads across each step; and of continental crust's thickness over what it settles to,
        // the share worn away.
        float spreading = 0.2F;
        float wearing = 0.03F;
        // Kilometres of continental crust that can hold themselves up; past it, crust flows out
        // under its own weight, so ranges that keep piling up widen into plateaus.
        float strongest = 65.0F;
    };

    enum class Crust : std::uint8_t { Oceanic, Continental };

    // The crust under a grid cell.
    struct Column {
        std::uint32_t plate = 0;
        Crust crust = Crust::Oceanic;
        float thickness = 7.0F; // kilometres
        // Steps since it formed, and since it was last piled up into a range or raised by an arc.
        float age = 0.0F;
        float raised = 0.0F;
        // Kilometres magma from a diving plate added to it last step.
        float arc = 0.0F;
    };

    struct Plate {
        DVec3 pole{0.0, 0.0, 1.0}; // unit, about which it turns
        double turn = 0.0;         // radians each step
    };

    // Ocean floor as it wells up, and continents as they settle when nothing piles them up.
    static constexpr float oceanic_thickness = 7.0F;
    static constexpr float continental_thickness = 35.0F;
    // Ocean floor an arc thickens this far floats as continent.
    static constexpr float arc_continent = 20.0F;

    // Precondition: 2 <= settings.plates <= Plates::max_count, and 0 < settings.continental < 1.
    Tectonics(std::uint64_t seed, double radius, Settings settings);
    // The given plates carrying the given crust, one column to each cell of `grid`.
    Tectonics(LevelGrid grid, std::vector<Plate> plates, std::vector<Column> columns,
              Settings settings);

    // Moves the plates on by a step.
    void step();

    [[nodiscard]] const LevelGrid& grid() const noexcept { return grid_; }
    [[nodiscard]] std::span<const Plate> plates() const noexcept { return plates_; }
    [[nodiscard]] std::span<const Column> columns() const noexcept { return columns_; }
    [[nodiscard]] int steps() const noexcept { return steps_; }
    // Square kilometres of continental crust times its thickness in kilometres.
    [[nodiscard]] double continental_volume() const;

private:
    // Carries each plate's crust on by a step; says which cells ocean floor dove under.
    [[nodiscard]] std::vector<bool> move();
    // Welds crust cut off from its plate as cells are resampled to the plate around it, so each
    // plate stays one piece.
    void keep_whole();
    void raise_arcs(const std::vector<bool>& diving);
    void collapse();
    void spread_and_wear();

    Settings settings_;
    LevelGrid grid_;
    std::vector<Plate> plates_;
    std::vector<Column> columns_;
    int steps_ = 0;
};

} // namespace hog::planet
