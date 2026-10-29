#pragma once

#include <hog/core/math.hpp>
#include <hog/planet/level_grid.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace hog::planet {

class Tectonics;
class Volume;

enum class CrustType : std::uint8_t { Oceanic, Continental };

// A column of crust as its plate carries it.
struct Crust {
    CrustType type = CrustType::Oceanic;
    float thickness = 7.0F; // kilometres
    // Millions of years since it formed, and since it was last piled up into a range.
    float age = 0.0F;
    float raised = 0.0F;

    friend bool operator==(const Crust&, const Crust&) = default;
};

// Of two crusts over one place, whether `one` stays on top of `other`: continents float over ocean
// floor, young warm floor over old cold floor, and the thicker of two continents over the thinner.
[[nodiscard]] bool stays_over(const Crust& one, const Crust& other) noexcept;

// How ocean floor diving under a plate thickens it: out from the trench across the plate over
// it, over so many places of the grid, by so many kilometres every million years.
struct Arcs {
    int reach = 3;
    float growth = 0.06F;
};

// How continental crust piled up into ranges bears its own weight: past `strongest` kilometres
// it flows out at once into the thinner continent around it, and short of that, what's piled over
// a continent's settled thickness spreads into it by `flow` of the difference every million
// years, so a range piled up along a line widens into a belt.
struct Ranges {
    float strongest = 65.0F;
    float flow = 0.02F;
};

// The crust as rigid plates on the sphere, each keeping its crust on a grid in its own frame and
// turning about its own pole. Crust is never carried from cell to cell, so a plate stays whole
// however far it turns: where two plates' crust lies over one place they've met, and where none
// does they've parted.
class Lithosphere {
public:
    struct Plate {
        DQuat frame{1.0, 0.0, 0.0, 0.0}; // turns the plate's own frame into the planet's
        DVec3 pole{0.0, 0.0, 1.0};       // unit, in the planet's frame
        double rate = 0.0;               // radians per million years
        // The cells of the grid in its own frame it has crust in, ascending, and that crust.
        std::vector<std::uint32_t> cells;
        std::vector<Crust> crust;
    };

    struct Found {
        std::uint32_t plate = 0;
        Crust crust;

        friend bool operator==(const Found&, const Found&) = default;
    };

    // Precondition: each plate's cells are of `grid`, ascending, with one crust to each.
    Lithosphere(LevelGrid grid, std::vector<Plate> plates);

    // Turns every plate on by `years` million years, and ages its crust as much.
    void advance(double years);
    // Fills every place the plates have parted with new ocean floor, joined to the plate whose
    // crust is nearest across the gap, so the ridge runs down the gap's middle.
    void spread();
    // Ocean floor lying under another plate's crust dives into the mantle and is gone, and the
    // plate over it thickens into an arc behind the trench.
    void subduct(double years, Arcs arcs = {});
    // Continental crust lying under another plate's is too light to dive: it's scraped off onto
    // the crust over it, piling that up into a range, which then settles over `years` as
    // `ranges` says.
    void collide(double years, Ranges ranges = {});

    // Ocean floor an arc thickens this far floats as continent.
    static constexpr float arc_continent = 20.0F;
    // Continents as they settle when nothing piles them up.
    static constexpr float settled_continent = 35.0F;

    [[nodiscard]] const LevelGrid& grid() const noexcept { return grid_; }
    [[nodiscard]] std::span<const Plate> plates() const noexcept { return plates_; }
    // Millions of years advanced.
    [[nodiscard]] double time() const noexcept { return time_; }

    // A unit direction in a plate's own frame as it lies now in the planet's, and back.
    [[nodiscard]] DVec3 to_planet(std::uint32_t plate, DVec3 own) const;
    [[nodiscard]] DVec3 to_plate(std::uint32_t plate, DVec3 at) const;

    // The crust a plate carries over a unit direction, if any.
    [[nodiscard]] std::optional<Crust> crust_of(std::uint32_t plate, DVec3 at) const;
    // The crust left on top over a unit direction, or none where the plates have parted. Ties go
    // to the lower plate, so the same plates always give the same crust.
    [[nodiscard]] std::optional<Found> top(DVec3 at) const;

private:
    // Where in a plate's cells is the crust it carries over a unit direction, if it has any.
    [[nodiscard]] std::optional<std::size_t> held(std::uint32_t plate, DVec3 at) const;
    // Of each two plates where they meet, whether the first's crust stays over the second's of
    // the same type, one flag to each pair in a row per plate.
    [[nodiscard]] std::vector<bool> overriding() const;
    // Another plate whose crust lies over a plate's crust of `type` at a unit direction, if any.
    [[nodiscard]] std::optional<std::uint32_t> overlying(std::uint32_t plate, CrustType type,
                                                         DVec3 at,
                                                         const std::vector<bool>& overrides) const;
    // Thickens the plate on top out from each of the `trenches`, places of the grid.
    void raise_arcs(std::span<const std::uint32_t> trenches, double years, Arcs arcs);

    LevelGrid grid_;
    std::vector<Plate> plates_;
    double time_ = 0.0;
};

// The plates and crust of `tectonics` as they stand, each plate's frame the planet's, with one of
// its steps taken as `step_years` million years.
[[nodiscard]] Lithosphere lithosphere_from(const Tectonics& tectonics, double step_years);

// Fills every column of `level` from the volume's bottom to the planet's radius with the crust on
// top there, basalt for ocean floor and granite for continents, and the mantle under it, in shells
// of `shell_level`. Where the plates have parted the mantle reaches the top. All crust tops out at
// the radius, since how high it floats depends on more than the crust alone.
void fill_lithosphere(Volume& volume, const Lithosphere& lithosphere, double radius, int level,
                      int shell_level);

} // namespace hog::planet
