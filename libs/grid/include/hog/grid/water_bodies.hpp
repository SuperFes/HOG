#pragma once

#include <hog/core/math.hpp>
#include <hog/core/result.hpp>
#include <hog/grid/tree.hpp>

#include <cstddef>
#include <map>
#include <span>
#include <variant>
#include <vector>

namespace hog::grid {

// How water rises and falls through the year: metres its highest stands over its level, which is
// its lowest, and when it's highest, as a share of the year from the March equinox.
struct Season {
    double rise = 0.0;
    double peak = 0.0;

    friend constexpr bool operator==(const Season&, const Season&) noexcept = default;
};

// Levels are metres from the planet's centre, places metres from it, flows cubic metres a second.

struct Sea {
    double level = 0.0;

    friend constexpr bool operator==(const Sea&, const Sea&) noexcept = default;
};

struct Lake {
    double level = 0.0;
    Season season;

    friend constexpr bool operator==(const Lake&, const Lake&) noexcept = default;
};

// A river along its course, head to foot, with where its water stands and how much runs at each
// point; between points both change evenly.
struct River {
    struct Point {
        DVec3 place{0.0};
        double level = 0.0;
        double flow = 0.0;

        friend constexpr bool operator==(const Point&, const Point&) noexcept = default;
    };
    // Where a place lies against the river, at the nearest point of its course.
    struct Along {
        double level = 0.0;
        double flow = 0.0;
        DVec3 downstream{0.0};  // unit
        double from_head = 0.0; // metres along the course
        double off = 0.0;       // metres from the course
    };

    std::vector<Point> course;
    Season season;

    // Precondition: two points or more, no two at one place.
    [[nodiscard]] Along along(DVec3 place) const noexcept;

    friend bool operator==(const River&, const River&) = default;
};

using Body = std::variant<Sea, Lake, River>;

// Every water body by the id the water tree marks its basin with. Ids are never 0, which marks no
// body, and whoever makes the bodies keeps each one's id from one bake to the next.
struct WaterBodies {
    static constexpr std::uint8_t format = 1;

    std::map<Water::Value, Body> bodies;

    // Little-endian: format, a varint count, then each body by id: its id as a varint, its kind
    // (0 sea, 1 lake, 2 river) and its numbers as doubles; a river's points after a varint count.
    [[nodiscard]] std::vector<std::byte> encode() const;
    // Fails, saying why, on bytes that aren't bodies, an id of 0, or a river of fewer than two
    // points.
    [[nodiscard]] static Result<WaterBodies> decode(std::span<const std::byte> bytes);

    friend bool operator==(const WaterBodies&, const WaterBodies&) = default;
};

} // namespace hog::grid
