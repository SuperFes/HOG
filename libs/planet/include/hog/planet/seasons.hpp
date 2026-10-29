#pragma once

#include <hog/planet/system.hpp>

namespace hog::planet {

// What sets the seasons at one time in deep time.
struct Seasons {
    double tilt = 0.0; // degrees
    double eccentricity = 0.0;
    // Longitude of the orbit's nearest point, in degrees from the March equinox, measured as the
    // planet's own (Earth's is 103 today, so the Sun is nearest in early January).
    double periapsis = 0.0;
};

// The seasons `years` from J2000, below 0 in the past, on a planet tilted `axial_tilt` degrees
// today.
[[nodiscard]] Seasons seasons_at(const System& system, double axial_tilt, double years) noexcept;

// Watts a square metre the stars at the centre send to the top of the air at `latitude` degrees,
// averaged over a day, when the sun stands `solar_longitude` degrees from the March equinox.
[[nodiscard]] double daily_sunlight(const System& system, const Seasons& seasons, double latitude,
                                    double solar_longitude) noexcept;

} // namespace hog::planet
