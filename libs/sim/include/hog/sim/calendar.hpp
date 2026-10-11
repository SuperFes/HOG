#pragma once

#include <hog/core/math.hpp>
#include <hog/core/time.hpp>

#include <cstdint>

namespace hog::sim {

// Astronomical seasons, starting at the March equinox in the north.
enum class Season : std::uint8_t { Spring, Summer, Autumn, Winter };

// Days, seasons and the sun from planet time. Days are the planet's own, but the year is Earth's,
// so equinoxes and solstices fall on their real dates. Directions are in the planet's frame:
// north is +Z, and longitude 0 lies along +X.
class Calendar {
public:
    // Preconditions: day_length > 0, 0 <= axial_tilt <= 90 degrees.
    Calendar(GameDuration day_length, double axial_tilt);

    [[nodiscard]] GameDuration day_length() const noexcept { return day_length_; }

    // Whole days since the Unix epoch.
    [[nodiscard]] std::int64_t day(GameTime time) const noexcept;
    // How far through the day it is at longitude 0, from 0 at midnight to just under 1.
    [[nodiscard]] double time_of_day(GameTime time) const noexcept;
    // The same at the longitude of `up`.
    [[nodiscard]] double local_time_of_day(GameTime time, DVec3 up) const noexcept;

    // The sun's ecliptic longitude in radians, from 0 at the March equinox.
    [[nodiscard]] static double solar_longitude(GameTime time) noexcept;
    // The season at `up`, which is reversed in the southern hemisphere.
    [[nodiscard]] static Season season(GameTime time, DVec3 up) noexcept;

    // Unit direction towards the sun.
    [[nodiscard]] DVec3 sun_direction(GameTime time) const noexcept;
    // Radians above the horizon at `up`, below zero at night.
    [[nodiscard]] double sun_elevation(GameTime time, DVec3 up) const noexcept;

private:
    GameDuration day_length_;
    double sin_tilt_;
};

} // namespace hog::sim
