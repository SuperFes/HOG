#pragma once

#include <hog/core/time.hpp>
#include <hog/jobs/job_system.hpp>
#include <hog/mapgen/options.hpp>
#include <hog/mapgen/render.hpp>
#include <hog/planet/geology.hpp>
#include <hog/sim/wildlife.hpp>

#include <cstdint>
#include <string>
#include <string_view>

namespace hog::mapgen {

// The ground, dimmed, warming from red to yellow as a share of a species' crowded density rises
// from 0 to 1.
[[nodiscard]] Rgb density_colour(Rgb ground, double share) noexcept;

// Where a species lives, smoothed between grid cells. Precondition: the species is in the ecology.
[[nodiscard]] Image render_wildlife(const planet::Geology& geology, const sim::Wildlife& wildlife,
                                    std::uint32_t species, double planet_radius, const View& view,
                                    int width, jobs::JobSystem& jobs);

// The file name for a view, species and time, such as planet-wildlife-20260101T0000-deer.png.
[[nodiscard]] std::string wildlife_file_name(const View& view, std::string_view species,
                                             GameTime time);

// Comma-separated totals: the time, each species, and mean forage and carrion over land in kg per
// km². The header names the columns.
[[nodiscard]] std::string wildlife_header(const sim::Ecology& ecology);
[[nodiscard]] std::string wildlife_row(const sim::Wildlife& wildlife);

} // namespace hog::mapgen
