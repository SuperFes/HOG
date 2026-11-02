#include <hog/globe/cell.hpp>
#include <hog/planet/projection.hpp>
#include <hog/server/weather_keeper.hpp>
#include <hog/sim/cell_grid.hpp>

#include <algorithm>
#include <utility>

namespace hog::server {
namespace {

[[nodiscard]] double seconds(GameDuration duration) {
    return std::chrono::duration<double>(duration).count();
}

[[nodiscard]] protocol::CellWeather shown(const weather::Column& column) {
    return {.cloud = column.cloud,
            .falling = column.falling,
            .snow = column.snow,
            .drift = column.drift,
            .wind = column.wind,
            .humidity = column.humidity,
            .fog_top = column.fog_top,
            .base = column.base,
            .top = column.top};
}

} // namespace

WeatherKeeper::WeatherKeeper(const globe::Generator& generator, GameDuration day, double tilt,
                             GameTime start, WeatherSettings settings)
    : settings_(settings), calendar_(day, tilt),
      air_(generator, seconds(day), settings.level, settings.layers),
      reached_(start - settings.spin_up) {
    const sim::CellGrid grid{settings.level};
    placed_.resize(air_.field().grid.columns());
    for (std::size_t column = 0; column < placed_.size(); ++column) {
        const DVec3 up =
            globe::center(globe::cell_at_index(settings.level, static_cast<std::uint64_t>(column)));
        placed_[column] = grid.index(planet::cell_at(up, settings.level));
    }
}

WeatherKeeper::~WeatherKeeper() {
    if (job_.valid()) {
        job_.wait();
    }
}

std::optional<protocol::Weather> WeatherKeeper::poll(GameTime now, std::chrono::milliseconds fade) {
    std::optional<protocol::Weather> made;
    if (job_.valid()) {
        if (job_.wait_for(std::chrono::seconds{0}) != std::future_status::ready) {
            return made;
        }
        job_.get();
        // While the air is still catching up with the clock there is nothing true to show.
        if (now - reached_ < settings_.interval) {
            made = sum(fade);
            latest_ = made;
        }
    }
    if (now - reached_ < settings_.interval) {
        return made;
    }
    const GameDuration run = std::min(now - reached_, settings_.longest);
    const GameTime from = reached_;
    reached_ += run;
    job_ = std::async(std::launch::async, [this, from, run] {
        air_.advance(seconds(run), [&](double elapsed) {
            return calendar_.sun_direction(from + std::chrono::duration_cast<GameDuration>(
                                                      std::chrono::duration<double>{elapsed}));
        });
    });
    return made;
}

protocol::Weather WeatherKeeper::sum(std::chrono::milliseconds fade) const {
    protocol::Weather weather{.level = static_cast<std::uint8_t>(settings_.level),
                              .fade = fade,
                              .cells = std::vector<protocol::CellWeather>(placed_.size())};
    for (std::size_t column = 0; column < placed_.size(); ++column) {
        weather.cells[placed_[column]] = shown(air_.column(column));
    }
    return weather;
}

} // namespace hog::server
