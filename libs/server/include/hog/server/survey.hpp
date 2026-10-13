#pragma once

#include <hog/core/result.hpp>
#include <hog/jobs/job_system.hpp>
#include <hog/planet/caves.hpp>
#include <hog/planet/geology.hpp>
#include <hog/probe/messages.hpp>

#include <cstdint>

namespace hog::server {

// Fails when the edges are out of order or off the planet, there are no samples or too many, or
// the depth is negative.
[[nodiscard]] Result<void> check_survey(const probe::Survey& survey);

// The cell level a survey samples at: cells about as small as the samples are apart where they're
// furthest apart, nearest the equator.
[[nodiscard]] int survey_level(const probe::Survey& survey, double planet_radius);

// The width of a river within `margin` metres of a cell on dry land, in whole metres rounded up,
// from 1 to 255, or 0 where there is none.
[[nodiscard]] std::uint8_t river_width(const planet::Geology& geology, planet::CellId cell,
                                       double margin);

// How far from a sample a survey looks for rivers: half the samples' spacing on a row, so every
// river crosses at least one.
[[nodiscard]] double river_margin(const probe::Survey& survey, double planet_radius,
                                  double latitude);

// The generated ground under a checked survey. It reads only the geology and its caves, which
// never change, so any thread may call it with its own jobs; changes made to the world since
// aren't shown.
[[nodiscard]] probe::Ground survey_ground(const planet::Caves& caves, double planet_radius,
                                          const probe::Survey& survey, jobs::JobSystem& jobs);

// Fails unless the point lies on the planet.
[[nodiscard]] Result<void> check_inspect(const probe::Inspect& inspect);

// The cell level ground is inspected at: about a metre across, finer than the generator's detail.
[[nodiscard]] int inspect_level(double planet_radius);

// The generated ground under a checked point. Like a survey, any thread may call it.
[[nodiscard]] probe::Column ground_column(const planet::Caves& caves, double planet_radius,
                                          const probe::Inspect& inspect);

// The caves under a cell, shallowest first, found a quarter of a metre apart.
[[nodiscard]] std::vector<probe::Cavity> cavities(const planet::Caves& caves, planet::CellId cell);

} // namespace hog::server
