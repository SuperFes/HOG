#pragma once

#include <hog/core/math.hpp>
#include <hog/core/time.hpp>
#include <hog/jobs/job_system.hpp>
#include <hog/mapgen/render.hpp>
#include <hog/probe/messages.hpp>
#include <hog/sim/calendar.hpp>
#include <hog/viewer/map_view.hpp>

#include <numbers>
#include <optional>

namespace hog::viewer {

// How far below the horizon the sun still lights the sky.
inline constexpr double twilight = 6.0 * std::numbers::pi / 180.0;
// How bright the night side stays, so the map under it can still be read.
inline constexpr double night_light = 0.4;

// How lit the ground is with the sun at `elevation` radians: 1 by day, 0 by night, easing between
// through twilight.
[[nodiscard]] double sunlight(double elevation) noexcept;

// The planet's calendar, or none if the server didn't send a usable day length and tilt.
[[nodiscard]] std::optional<sim::Calendar> calendar_of(const probe::Welcome& welcome);

// Where the sun is overhead on the planet map, 0 to 1 across and down.
[[nodiscard]] DVec2 subsolar_point(const sim::Calendar& calendar, GameTime time) noexcept;

// Darkens the night side of an image of the shown part of the map, as of `time`.
void shade_night(mapgen::Image& image, const MapRect& shown, const sim::Calendar& calendar,
                 GameTime time, jobs::JobSystem& jobs);

} // namespace hog::viewer
