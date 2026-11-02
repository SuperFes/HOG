#pragma once

#include <hog/core/math.hpp>
#include <hog/globe/world.hpp>

#include <optional>

namespace hog::globe {

// The steepest slope, in radians from level, someone standing with their feet at `feet` (metres
// from the planet's centre) is held on: what the bare ground gives, or the grain lying on it, as
// wet as the water in them makes it. None where no ground is found just under the feet.
[[nodiscard]] std::optional<double> grip_at(const World& world, DVec3 feet);

} // namespace hog::globe
