#pragma once

#include <hog/core/math.hpp>

namespace hog::render {

// Gusts are patches of stronger wind drifting over the ground and changing shape as they go, a
// field in the planet's frame that gusts.slang reads.

// Metres the gusts' field repeats over, as gusts.slang's.
inline constexpr double gust_period = 2'048.0;

// How fast the gusts' field moves, m/s, under `wind` near the ground where `up` is up: along the
// wind, faster than it blows there, as gusts are brought down from the faster air above; and up
// through the ground, which turns the patches' shapes as they go.
[[nodiscard]] DVec3 gust_drift(DVec3 wind, DVec3 up) noexcept;

// Where the gusts' field lies from `eye` once it has drifted `drifted`, within its period, so its
// precision is kept.
[[nodiscard]] DVec3 gust_anchor(const WorldPosition& eye, DVec3 drifted) noexcept;

} // namespace hog::render
