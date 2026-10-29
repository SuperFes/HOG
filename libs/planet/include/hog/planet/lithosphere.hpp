#pragma once

#include <hog/core/math.hpp>
#include <hog/planet/level_grid.hpp>

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
    };

    // Precondition: each plate's cells are of `grid`, ascending, with one crust to each.
    Lithosphere(LevelGrid grid, std::vector<Plate> plates);

    // Turns every plate on by `years` million years, and ages its crust as much.
    void advance(double years);

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
