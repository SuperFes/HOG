#pragma once

#include <hog/core/image.hpp>
#include <hog/flora/woods.hpp>
#include <hog/globe/generator.hpp>
#include <hog/globe/land.hpp>
#include <hog/globe/material.hpp>

#include <array>
#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace hog::census {

// What the world shows over one cell of a level's grid.
struct Spot {
    double ground = 0.0;                        // metres over the radius
    std::optional<double> water;                // metres over the radius open water stands at
    globe::Material top = globe::Material::Air; // what the ground's top is made of
    // Where surveyed, the woods at its middle.
    // NOLINTNEXTLINE(readability-redundant-member-init): lets designated initializers omit it
    flora::Woods woods = {};
};

// Water standing off the sea, joined cell to cell.
struct Body {
    double area = 0.0;     // km²
    double level = 0.0;    // metres over the sea, or the radius on a world without one; its lowest
    double fall = 0.0;     // metres its level falls across it
    double deepest = 0.0;  // metres
    double latitude = 0.0; // degrees, at its deepest
    double longitude = 0.0;
};

// What a world holds, counted cell by cell over a level's grid.
struct Census {
    int level = 0;
    double cell = 0.0;   // metres across a cell
    double area = 0.0;   // km²
    double land = 0.0;   // km² not under water
    double sea = 0.0;    // km² under the sea
    double lowest = 0.0; // metres over the sea, the ground's
    double highest = 0.0;
    // Inland water, largest first: still where its level stays within `still` of its lowest.
    std::vector<Body> lakes;
    std::vector<Body> running;
    // km² of land each material tops.
    std::array<double, globe::material_count> tops{};
    // Where the woods were surveyed: km² under crowns, and under each kind's; and the highest
    // ground a tenth or more under them, metres over the sea.
    bool woods = false;
    double wooded = 0.0;
    std::array<double, flora::kinds> kinds{};
    double treeline = 0.0;
};

// Metres a body's level may vary and it still stands still.
inline constexpr double still = 0.1;

// Counts what `spots` show, each the cell at its index_of on `level`, on a world of `radius`
// whose sea stands at `sea` metres over it.
[[nodiscard]] Census count(int level, std::span<const Spot> spots, double radius,
                           std::optional<double> sea);

// What the generator makes of each cell on `level`, read through its columns, and with `woods`
// the woods at each one's middle.
[[nodiscard]] std::vector<Spot> survey(const globe::Generator& generator, int level,
                                       const globe::EachOf& each = {}, bool woods = false);

// The woods surveyed over the whole planet, `width` pixels round by half that from pole to pole:
// each kind in its own colour, as thick as the crowns cover the ground, over bare ground, ice
// and water.
[[nodiscard]] Image woods_map(int level, std::span<const Spot> spots, int width,
                              std::optional<double> sea);

// The census as lines to read, the `lakes` largest of each kind of water listed.
[[nodiscard]] std::string report(const Census& census, std::size_t lakes = 10);

} // namespace hog::census
