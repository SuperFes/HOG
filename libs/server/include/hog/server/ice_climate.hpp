#pragma once

#include <hog/flow/region.hpp>
#include <hog/globe/cell.hpp>
#include <hog/globe/planet.hpp>
#include <hog/weather/snowpack.hpp>

#include <functional>

namespace hog::server {

// The year-round climate of a column at the planet's reference level: what the weather gives it.
using ClimateAt = std::function<weather::Climate(const globe::Cell& column)>;

// The flow's ice balance for a planet, read from the weather's climate: each column's climate
// taken to the height of the ground it asks about.
[[nodiscard]] flow::Region::IceBalance ice_balance(const globe::Planet& planet, ClimateAt climate);

} // namespace hog::server
