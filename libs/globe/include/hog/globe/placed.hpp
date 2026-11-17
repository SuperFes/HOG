#pragma once

#include <hog/core/math.hpp>
#include <hog/globe/generator.hpp>
#include <hog/globe/material.hpp>

#include <array>
#include <cstdint>

namespace hog::globe {

// A thing placed on or in the ground rather than shaped into it, held apart as an object: a
// fallen rock, a crag, a vent. Solid ones are boxes between three pairs of faces, their edges
// rounded off; a vent is a point.
struct Placed {
    enum class Kind : std::uint8_t { Rock, Crag, Vent, Pool };
    Kind kind = Kind::Rock;
    std::uint64_t key = 0; // the same for the same thing, whatever the detail it's placed at
    Material material = Material::Air;
    DVec3 middle{0.0};            // metres from the planet's centre
    std::array<DVec3, 3> faces{}; // unit, square to each pair of faces
    DVec3 half{0.0};              // metres from the middle to each pair
    double rounding = 0.0;        // metres its edges and corners are rounded off

    // Metres from `point` to its surface, negative inside: true where its faces are square to
    // each other, close where they're near it.
    [[nodiscard]] double distance(DVec3 point) const noexcept;
};

[[nodiscard]] Placed placed_rock(const Rock& rock) noexcept;

} // namespace hog::globe
