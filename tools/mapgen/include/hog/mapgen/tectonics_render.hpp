#pragma once

#include <hog/jobs/job_system.hpp>
#include <hog/mapgen/colour.hpp>
#include <hog/mapgen/options.hpp>
#include <hog/mapgen/render.hpp>
#include <hog/planet/tectonics.hpp>

#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace hog::mapgen {

// Ocean floor blue, paler the younger; continents green, rising through tan to white the thicker
// they've piled up.
[[nodiscard]] Rgb crust_colour(const planet::Tectonics::Column& column) noexcept;

// The sea blue, darker the deeper; the land green, rising through tan to white the higher.
[[nodiscard]] Rgb relief_colour(double above_sea) noexcept;

// The crust as it lies, with the edges between plates darkened.
[[nodiscard]] Image render_tectonics(const planet::Tectonics& tectonics, double planet_radius,
                                     const View& view, int width, jobs::JobSystem& jobs);
// The crust's fixed-point `heights` against the sea, plate edges darkened the same.
[[nodiscard]] Image render_relief(const planet::Tectonics& tectonics,
                                  std::span<const std::int64_t> heights, std::int64_t sea_level,
                                  double planet_radius, const View& view, int width,
                                  jobs::JobSystem& jobs);

// The file name for a view, kind of map and step, such as planet-tectonics-0040.png.
[[nodiscard]] std::string evolution_file_name(const View& view, std::string_view kind, int step);

} // namespace hog::mapgen
