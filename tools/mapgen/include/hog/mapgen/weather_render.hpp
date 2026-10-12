#pragma once

#include <hog/core/time.hpp>
#include <hog/jobs/job_system.hpp>
#include <hog/mapgen/options.hpp>
#include <hog/mapgen/render.hpp>
#include <hog/planet/geology.hpp>
#include <hog/sim/weather.hpp>

#include <cstdint>
#include <string>
#include <string_view>

namespace hog::mapgen {

enum class WeatherMap : std::uint8_t {
    Sky, // the ground under clouds, rain, snow and storms
    Air, // temperature with isobars and fronts
};

[[nodiscard]] std::string_view to_string(WeatherMap map) noexcept;

// What falls, in mm per hour.
struct Falling {
    double rain = 0.0;
    double snow = 0.0;
    double hail = 0.0;
};

// The ground under a share of cloud: white with snow, blue with rain, violet with storms (0 to 1)
// and yellow with hail.
[[nodiscard]] Rgb sky_colour(Rgb ground, double cover, Falling falling, double storm) noexcept;

// Temperature in °C, reddened along warm fronts and blued along cold ones; fronts run from -1
// (cold) to 1 (warm).
[[nodiscard]] Rgb air_colour(double temperature, double front) noexcept;

// Air maps draw a line between every this many hPa.
inline constexpr double isobar_spacing = 4.0;

[[nodiscard]] Image render_weather(const planet::Geology& geology, const sim::Weather& weather,
                                   double planet_radius, const View& view, WeatherMap map,
                                   int width, jobs::JobSystem& jobs);

// The file name for a view, map and time, such as planet-weather-20260101T0600-sky.png.
[[nodiscard]] std::string weather_file_name(const View& view, WeatherMap map, GameTime time);

} // namespace hog::mapgen
