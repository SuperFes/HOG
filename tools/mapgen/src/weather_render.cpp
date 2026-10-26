#include <hog/mapgen/weather_render.hpp>
#include <hog/planet/noise.hpp>
#include <hog/planet/projection.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <format>
#include <span>
#include <vector>

namespace hog::mapgen {

namespace {

constexpr Rgb cloud_white{235, 238, 242};
constexpr Rgb rain_blue{40, 90, 220};
constexpr Rgb snow_white{250, 252, 255};
constexpr Rgb storm_violet{150, 50, 190};
constexpr Rgb hail_yellow{250, 225, 40};
constexpr Rgb warm_red{220, 40, 30};
constexpr Rgb cold_blue{30, 70, 230};
constexpr Rgb isobar_grey{45, 45, 50};

// Rates that colour a pixel fully, in mm per hour.
constexpr double heavy_rain = 4.0;
constexpr double heavy_snow = 2.0;
constexpr double heavy_hail = 0.5;

double at(std::span<const float> field, std::uint32_t index) {
    return static_cast<double>(field[index]);
}

void put(Image& image, int x, int y, Rgb colour) {
    const auto offset = 3 * ((static_cast<std::size_t>(y) * static_cast<std::size_t>(image.width)) +
                             static_cast<std::size_t>(x));
    std::ranges::copy(colour, image.pixels.begin() + static_cast<std::ptrdiff_t>(offset));
}

// Lines where the pressure crosses a multiple of the spacing between neighbouring pixels.
void draw_isobars(Image& image, const std::vector<float>& pressure) {
    const auto band = [&](int x, int y) {
        return std::floor(
            static_cast<double>(
                pressure[(static_cast<std::size_t>(y) * static_cast<std::size_t>(image.width)) +
                         static_cast<std::size_t>(x)]) /
            isobar_spacing);
    };
    for (int y = 0; y + 1 < image.height; ++y) {
        for (int x = 0; x + 1 < image.width; ++x) {
            if (band(x, y) != band(x + 1, y) || band(x, y) != band(x, y + 1)) {
                put(image, x, y, isobar_grey);
            }
        }
    }
}

} // namespace

std::string_view to_string(WeatherMap map) noexcept {
    switch (map) {
    case WeatherMap::Sky:
        return "sky";
    case WeatherMap::Air:
        return "air";
    }
    return "unknown";
}

Rgb sky_colour(Rgb ground, double cover, Falling falling, double storm) noexcept {
    Rgb colour = mix(ground, cloud_white, 0.85 * cover);
    colour = mix(colour, rain_blue, falling.rain / heavy_rain);
    colour = mix(colour, snow_white, falling.snow / heavy_snow);
    colour = mix(colour, storm_violet, storm);
    return mix(colour, hail_yellow, falling.hail / heavy_hail);
}

Rgb air_colour(double temperature, double front) noexcept {
    const Rgb base = temperature_colour(static_cast<std::int64_t>(
        std::lround(temperature * static_cast<double>(planet::fixed_one))));
    return front >= 0.0 ? mix(base, warm_red, front) : mix(base, cold_blue, -front);
}

Image render_weather(const planet::Geology& geology, const sim::Weather& weather,
                     double planet_radius, const View& view, WeatherMap map, int width,
                     jobs::JobSystem& jobs) {
    const ImageSize size = image_size(view, width);
    Image image{.width = size.width,
                .height = size.height,
                .pixels = std::vector<std::uint8_t>(3 * static_cast<std::size_t>(size.width) *
                                                    static_cast<std::size_t>(size.height))};
    std::vector<float> pressure(static_cast<std::size_t>(size.width) *
                                static_cast<std::size_t>(size.height));
    const sim::CellGrid& grid = weather.grid();
    const sim::WeatherFields& fields = weather.fields();
    const double per_hour = 3'600.0 / std::chrono::duration<double>(weather.config().step).count();
    const auto colour_at = [&](DVec3 direction, float& pressure_here) -> Rgb {
        const auto field = [&](const std::vector<float>& values) {
            return grid.interpolate(direction,
                                    [&](std::uint32_t index) { return at(values, index); });
        };
        if (map == WeatherMap::Air) {
            pressure_here = static_cast<float>(field(fields.pressure));
            return air_colour(field(fields.temperature), field(fields.front));
        }
        const Rgb ground = height_colour(geology.terrain().elevation(planet::cell_at(direction)));
        const auto cover =
            static_cast<double>(sim::cloud_cover(static_cast<float>(field(fields.cloud))));
        const Falling falling{.rain = field(fields.rain) * per_hour,
                              .snow = field(fields.snow) * per_hour,
                              .hail = field(fields.hail) * per_hour};
        return sky_colour(ground, cover, falling, field(fields.storm));
    };
    jobs.parallel_for(static_cast<std::uint32_t>(size.height), 1,
                      [&](jobs::Range rows, std::uint32_t /*thread*/) {
                          for (std::uint32_t row = rows.begin; row < rows.end; ++row) {
                              const auto y = static_cast<int>(row);
                              for (int x = 0; x < size.width; ++x) {
                                  const auto pixel = (static_cast<std::size_t>(y) *
                                                      static_cast<std::size_t>(size.width)) +
                                                     static_cast<std::size_t>(x);
                                  put(image, x, y,
                                      colour_at(pixel_direction(view, size, x, y, planet_radius),
                                                pressure[pixel]));
                              }
                          }
                      });
    if (map == WeatherMap::Air) {
        draw_isobars(image, pressure);
    }
    return image;
}

std::string weather_file_name(const View& view, WeatherMap map, GameTime time) {
    // Same prefix as the layer maps, so a view's files sort together.
    const std::string layer_name = file_name(view, Layer::Height);
    const std::string prefix = layer_name.substr(0, layer_name.rfind('-'));
    const auto seconds = std::chrono::floor<std::chrono::minutes>(
        std::chrono::sys_time<GameDuration>{time.time_since_epoch()});
    return std::format("{}-weather-{:%Y%m%dT%H%M}-{}.png", prefix, seconds, to_string(map));
}

} // namespace hog::mapgen
