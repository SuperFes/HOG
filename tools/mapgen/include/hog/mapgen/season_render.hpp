#pragma once

#include <hog/jobs/job_system.hpp>
#include <hog/mapgen/options.hpp>
#include <hog/mapgen/render.hpp>
#include <hog/planet/geology.hpp>
#include <hog/sim/forest.hpp>
#include <hog/sim/weather.hpp>

#include <chrono>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace hog::mapgen {

enum class SeasonMap : std::uint8_t {
    Temperature, // recent surface temperature
    Rain,        // recent rain and snow
    Land,        // soil water and snow, with rivers by how full they run
    Forest,      // plants by how grown they are, and fires
};

inline constexpr int season_map_count = static_cast<int>(SeasonMap::Forest) + 1;

[[nodiscard]] std::string_view to_string(SeasonMap map) noexcept;

// Rain and snow in mm per day, from parched tan to deep blue.
[[nodiscard]] Rgb rain_colour(double per_day) noexcept;

// The ground by how full its soil is (0 to 1), whitening under snow in mm of water.
[[nodiscard]] Rgb soil_colour(double soil, double snow) noexcept;

// A river by its flow over its usual: fading into the ground in a drought, muddying in flood.
[[nodiscard]] Rgb flowing_colour(Rgb ground, Rgb river, double ratio) noexcept;

// Plants by their share of what their biome holds, from bare earth to fully grown, glowing by
// the share of the cell alight.
[[nodiscard]] Rgb forest_colour(Rgb grown, double share, double fire) noexcept;

// Precondition: the forest is on the weather's grid.
[[nodiscard]] Image render_season(const planet::Geology& geology, const sim::Weather& weather,
                                  const sim::Forest& forest, double planet_radius, const View& view,
                                  SeasonMap map, int width, jobs::JobSystem& jobs);

// Frames side by side, `columns` across, filling rows from the top left. Precondition: at least
// one frame, all the same size, and a positive column count.
[[nodiscard]] Image contact_sheet(std::span<const Image> frames, int columns);

// Such as planet-seasons-202607-land.png for a month's frame, and planet-seasons-land.png for the
// sheet of the year.
[[nodiscard]] std::string season_file_name(const View& view, SeasonMap map,
                                           std::chrono::year_month month);
[[nodiscard]] std::string season_sheet_name(const View& view, SeasonMap map);

} // namespace hog::mapgen
