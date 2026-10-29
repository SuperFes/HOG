#pragma once

#include <hog/core/math.hpp>

#include <span>

namespace hog::waves {

// The tide: the sea drawn up under each moon and star and on the far side from it, so it rises
// and falls twice a day; with the pulls in step it runs high, springs, and out of step low,
// neaps. A body north or south of the equator draws one of the day's two tides higher than the
// other.

// A body raising the tide: its direction from the planet's centre, and the metres it raises the
// sea now where it raises it most.
struct Pull {
    DVec3 toward{};
    double height = 0.0;
};

// How high the tide stands above the sea's mean at `up`, a direction from the planet's centre,
// under `pulls`. Directions are as sim::Calendar's: north is +Z and longitude 0 lies along +X.
[[nodiscard]] double tide_at(DVec3 up, std::span<const Pull> pulls) noexcept;

} // namespace hog::waves
