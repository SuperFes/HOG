#pragma once

#include <hog/core/time.hpp>
#include <hog/planet/ephemeris.hpp>
#include <hog/waves/tide.hpp>

#include <vector>

namespace hog::client {

// What draws the tide at `time`: every moon and star of the system, where it stands.
[[nodiscard]] std::vector<waves::Pull> tide_pulls(const planet::Ephemeris& ephemeris,
                                                  GameTime time);

} // namespace hog::client
