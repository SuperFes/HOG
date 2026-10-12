#pragma once

#include <hog/core/result.hpp>
#include <hog/jobs/job_system.hpp>
#include <hog/planet/geology.hpp>
#include <hog/probe/messages.hpp>

namespace hog::server {

// Fails when the edges are out of order or off the planet, there are no samples or too many, or
// the depth is negative.
[[nodiscard]] Result<void> check_survey(const probe::Survey& survey);

// The cell level a survey samples at: cells about as small as the samples are apart where they're
// furthest apart, nearest the equator.
[[nodiscard]] int survey_level(const probe::Survey& survey, double planet_radius);

// The generated ground under a checked survey. It reads only the geology, which never changes, so
// any thread may call it with its own jobs; changes made to the world since aren't shown.
[[nodiscard]] probe::Ground survey_ground(const planet::Geology& geology, double planet_radius,
                                          const probe::Survey& survey, jobs::JobSystem& jobs);

} // namespace hog::server
