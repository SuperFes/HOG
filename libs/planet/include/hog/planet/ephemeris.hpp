#pragma once

#include <hog/core/math.hpp>
#include <hog/core/time.hpp>
#include <hog/planet/system.hpp>

#include <cstddef>

namespace hog::planet {

// Watts a square metre the Sun sends at Earth's mean distance.
inline constexpr double solar_constant = 1'361.0;

// Where a body on `orbit` is, `periods` of it after J2000: metres from what it orbits, with +X
// towards the March equinox and +Z north of the planet's orbit.
[[nodiscard]] DVec3 orbit_position(const Orbit& orbit, double periods) noexcept;

// A star's light in linear RGB, from a black body at `temperature` kelvin, so balanced that the
// Sun's is white, as eyes used to it see it.
[[nodiscard]] DVec3 star_colour(double temperature) noexcept;

// Where the bodies of a system stand seen from the planet. Positions are metres from its centre
// in its own turning frame, as sim::Calendar's: north is +Z, and longitude 0 lies along +X. It
// turns once a day against the system's centre, so the sun keeps the calendar's hours but for
// the slip an eccentric, tilted orbit gives it.
class Ephemeris {
public:
    // Preconditions: day_length > 0, 0 <= axial_tilt <= 90 degrees.
    Ephemeris(System system, GameDuration day_length, double axial_tilt);

    [[nodiscard]] const System& system() const noexcept { return system_; }

    [[nodiscard]] DVec3 star(std::size_t index, GameTime time) const;
    [[nodiscard]] DVec3 moon(std::size_t index, GameTime time) const;
    [[nodiscard]] DVec3 planet(std::size_t index, GameTime time) const;

    // Watts a square metre the star sends at the top of the air, on ground facing it.
    [[nodiscard]] double irradiance(std::size_t index, GameTime time) const;

private:
    [[nodiscard]] DVec3 planet_around_centre(GameTime time) const noexcept;
    [[nodiscard]] DVec3 turned(DVec3 ecliptic, GameTime time) const noexcept;

    System system_;
    GameDuration day_length_;
    double tilt_; // radians
};

} // namespace hog::planet
