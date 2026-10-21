#pragma once

#include <hog/core/math.hpp>
#include <hog/core/time.hpp>

namespace hog::waves {

// The tide: the sea rising and falling twice a day as a moon and the sun draw it, on the planet's
// own day. With the moon's tide and the sun's in step it runs high, springs; out of step, low,
// neaps.

// A moon's day lasts this many of the planet's, as Earth's moon's does of Earth's: the moon falls
// behind the sun a little each day, and springs come round about every fifteen.
inline constexpr double lunar_day = 1.0351;
// Metres the moon's tide and the sun's each rise above the sea's mean where they rise highest.
inline constexpr double lunar_tide = 0.6;
inline constexpr double solar_tide = 0.28;

// How high the tide stands above the sea's mean at `up`, a direction from the planet's centre, at
// `time`, on a planet whose day lasts `day`. Directions are as sim::Calendar's: north is +Z and
// longitude 0 lies along +X.
[[nodiscard]] double tide_at(DVec3 up, GameTime time, GameDuration day) noexcept;

} // namespace hog::waves
