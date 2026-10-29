#pragma once

#include <hog/jobs/job_system.hpp>
#include <hog/mapgen/colour.hpp>
#include <hog/mapgen/options.hpp>
#include <hog/mapgen/render.hpp>
#include <hog/planet/tectonics.hpp>

#include <string>

namespace hog::mapgen {

// Ocean floor blue, paler the younger; continents green, rising through tan to white the thicker
// they've piled up.
[[nodiscard]] Rgb crust_colour(const planet::Tectonics::Column& column) noexcept;

// The crust as it lies, with the edges between plates darkened.
[[nodiscard]] Image render_tectonics(const planet::Tectonics& tectonics, double planet_radius,
                                     const View& view, int width, jobs::JobSystem& jobs);

// The file name for a view and step, such as planet-tectonics-0040.png.
[[nodiscard]] std::string tectonics_file_name(const View& view, int step);

} // namespace hog::mapgen
