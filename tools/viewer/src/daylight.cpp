#include <hog/viewer/daylight.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace hog::viewer {

namespace {

void shade_row(mapgen::Image& image, const MapRect& shown, DVec3 sun, int y) {
    for (int x = 0; x < image.width; ++x) {
        const DVec3 up = map_direction(pixel_point(shown, image.width, image.height, x, y));
        const double light = sim::sunlight(std::asin(std::clamp(glm::dot(sun, up), -1.0, 1.0)));
        if (light >= 1.0) {
            continue;
        }
        const double scale = night_light + ((1.0 - night_light) * light);
        const auto offset =
            3 * ((static_cast<std::size_t>(y) * static_cast<std::size_t>(image.width)) +
                 static_cast<std::size_t>(x));
        for (std::size_t channel = 0; channel < 3; ++channel) {
            std::uint8_t& value = image.pixels[offset + channel];
            value = static_cast<std::uint8_t>(std::lround(value * scale));
        }
    }
}

} // namespace

std::optional<sim::Calendar> calendar_of(const probe::Welcome& welcome) {
    const double tilt = welcome.axial_tilt;
    if (welcome.day_length <= GameDuration::zero() || std::isnan(tilt) || tilt < 0.0 ||
        tilt > 90.0) {
        return std::nullopt;
    }
    return sim::Calendar{welcome.day_length, welcome.axial_tilt};
}

DVec2 subsolar_point(const sim::Calendar& calendar, GameTime time) noexcept {
    return direction_point(calendar.sun_direction(time));
}

void shade_night(mapgen::Image& image, const MapRect& shown, const sim::Calendar& calendar,
                 GameTime time, jobs::JobSystem& jobs) {
    const DVec3 sun = calendar.sun_direction(time);
    jobs.parallel_for(static_cast<std::uint32_t>(image.height), 1,
                      [&](jobs::Range rows, std::uint32_t /*thread*/) {
                          for (std::uint32_t row = rows.begin; row < rows.end; ++row) {
                              shade_row(image, shown, sun, static_cast<int>(row));
                          }
                      });
}

} // namespace hog::viewer
