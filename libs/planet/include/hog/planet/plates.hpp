#pragma once

#include <hog/core/math.hpp>
#include <hog/planet/level_grid.hpp>

#include <cstdint>
#include <span>
#include <vector>

namespace hog::planet {

// The crust broken into plates on a coarse grid, each turning about its own pole. Where two meet,
// how they move against each other says whether the boundary closes, parts or slides.
class Plates {
public:
    static constexpr int max_count = 64;

    struct Plate {
        DVec3 origin; // unit direction it grew from
        // Radians per million years about the planet's middle: the ground at unit direction `p`
        // moves at spin × p times the radius.
        DVec3 spin;
        // How readily it spreads as the plates grow; larger plates spread more readily.
        double growth = 1.0;
    };

    // The nearest boundary to a grid cell, and how the plates move across it.
    struct Boundary {
        std::int64_t distance = 0; // fixed-point metres
        std::uint32_t other = 0;   // the plate across it
        // Metres per million years: how fast the plates close, negative where they part, and how
        // fast they slide past each other.
        double closing = 0.0;
        double sliding = 0.0;
    };

    // Precondition: 2 <= count <= max_count, and `radius` is the grid's.
    Plates(std::uint64_t seed, LevelGrid grid, double radius, int count);
    // The given plates, grown over the grid.
    Plates(std::uint64_t seed, LevelGrid grid, double radius, std::vector<Plate> plates);

    [[nodiscard]] const LevelGrid& grid() const noexcept { return grid_; }
    [[nodiscard]] std::span<const Plate> plates() const noexcept { return plates_; }
    // The plate under each grid cell.
    [[nodiscard]] std::span<const std::uint32_t> plate() const noexcept { return plate_; }
    // The nearest boundary to each grid cell.
    [[nodiscard]] std::span<const Boundary> boundaries() const noexcept { return boundaries_; }
    // Metres per million years the ground moves at a unit direction on a plate.
    [[nodiscard]] DVec3 velocity(std::uint32_t plate, DVec3 at) const;

private:
    void grow(std::uint64_t seed);
    void find_boundaries();

    LevelGrid grid_;
    double radius_;
    std::vector<Plate> plates_;
    std::vector<std::uint32_t> plate_;
    std::vector<Boundary> boundaries_;
};

} // namespace hog::planet
