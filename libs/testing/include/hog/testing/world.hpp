#pragma once

#include <hog/globe/generator.hpp>
#include <hog/globe/planet.hpp>

namespace hog::testing {

// The planet tests stand on: small enough to form in seconds, yet with sea, land, mountains,
// rivers and lakes. Fixed here, apart from the planet the game is played on, so no change to that
// moves a test.
[[nodiscard]] globe::Planet small_planet();

// The small planet, formed once for the whole test run.
[[nodiscard]] const globe::Generator& small_world();

} // namespace hog::testing
