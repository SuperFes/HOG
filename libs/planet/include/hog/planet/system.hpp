#pragma once

#include <cstdint>
#include <optional>
#include <vector>

namespace hog::planet {

inline constexpr double astronomical_unit = 1.495'978'707e11; // metres

// Where a body runs round what it orbits, as it stood at the start of 2000 (J2000). Angles are in
// degrees against the planet's orbit and the March equinox, as for Earth's; a moon's are against
// the same plane, as for Earth's moon.
struct Orbit {
    double distance = 0.0;     // metres, half the long axis; 0 sits at the centre
    double eccentricity = 0.0; // from 0, a circle, to below 1
    double inclination = 0.0;
    double node = 0.0;      // longitude of the ascending node
    double periapsis = 0.0; // longitude of the nearest point
    double longitude = 0.0; // mean longitude at J2000
    // Set, not worked out from masses: the planet is small for the game's sake, so its moons
    // would crawl if they kept Kepler's law to its true mass. Stars and planets count it in
    // Earth's years, which the calendar keeps; moons in the planet's own days, which the tides
    // and phases follow.
    double period = 1.0;

    bool operator==(const Orbit&) const = default;
};

// The stars orbit the system's centre, and so does the planet: a close pair is orbited together,
// and a far companion is one star at the centre and the other far out.
struct Star {
    double luminosity = 1.0;      // the Sun's
    double temperature = 5'772.0; // kelvin, which sets its colour
    double radius = 6.957e8;      // metres
    double tide = 0.28;           // metres it raises the sea where it raises it most
    Orbit orbit{};

    bool operator==(const Star&) const = default;
};

struct Moon {
    double radius = 1.7374e6; // metres
    double albedo = 0.12;
    double tide = 0.6; // metres
    Orbit orbit{.distance = 3.844e8,
                .eccentricity = 0.0549,
                .inclination = 5.145,
                .node = 125.08,
                .periapsis = 83.23,
                .longitude = 218.32,
                .period = 27.3217};

    bool operator==(const Moon&) const = default;
};

// A flat band in the plane of the equator.
struct Rings {
    double inner = 1.5; // planet radii from its centre
    double outer = 2.3;
    double opacity = 0.5; // share of light through them that they stop
    double albedo = 0.5;

    bool operator==(const Rings&) const = default;
};

// Another planet of the system, seen from this one as a light.
struct Wanderer {
    double radius = 0.0; // metres
    double albedo = 0.0;
    Orbit orbit{};

    bool operator==(const Wanderer&) const = default;
};

// Only what shapes the land counts: the pressure and carbon dioxide hold heat, which the climate
// reads, and oxygen feeds fire.
struct Air {
    double pressure = 101'325.0; // pascals at sea level
    double oxygen = 0.2095;      // share by volume
    double carbon_dioxide = 2.8e-4;

    bool operator==(const Air&) const = default;
};

// Earth's naked-eye planets, as they stood at J2000.
[[nodiscard]] std::vector<Wanderer> earths_planets();

// The planet's place among its stars, moons and neighbours, Earth's by default.
struct System {
    std::vector<Star> stars{Star{}};
    Orbit orbit{.distance = 1.000'002'61 * astronomical_unit,
                .eccentricity = 0.016'711'23,
                .periapsis = 102.937'68,
                .longitude = 100.464'57,
                .period = 1.0};
    std::vector<Moon> moons{Moon{}};
    std::optional<Rings> rings;
    std::vector<Wanderer> planets = earths_planets();
    // Set, not worked out from the size, so everything on the small planet behaves as on Earth.
    double gravity = 9.81; // metres a second squared
    Air air{};
    // The world's water as an even layer over the whole planet, as Earth's would stand.
    double water_depth = 2'620.0; // metres

    bool operator==(const System&) const = default;
};

// Changes whenever anything in the system does.
[[nodiscard]] std::uint64_t system_id(const System& system) noexcept;

} // namespace hog::planet
