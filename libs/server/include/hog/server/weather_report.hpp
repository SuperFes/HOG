#pragma once

#include <hog/protocol/messages.hpp>
#include <hog/sim/cell_grid.hpp>
#include <hog/sim/weather.hpp>

#include <chrono>

namespace hog::server {

// The weather as players are sent it, to fade to over `fade`: the grid's own cells, or the
// cells as fine as may be sent, each averaged over the area under it. What fell over the last
// step is sent as a rate, and the weather drifts with the wind the simulation moves it with.
// Precondition: one value per cell of `grid` in each of the fields.
[[nodiscard]] protocol::Weather weather_report(const sim::CellGrid& grid,
                                               const sim::WeatherFields& fields,
                                               const sim::WeatherConfig& config,
                                               std::chrono::milliseconds fade);

} // namespace hog::server
