#include <hog/core/bytes.hpp>
#include <hog/globe/cell.hpp>
#include <hog/planet/projection.hpp>
#include <hog/server/weather_keeper.hpp>
#include <hog/sim/cell_grid.hpp>
#include <hog/waves/waves.hpp>
#include <hog/weather/state.hpp>

#include <algorithm>
#include <utility>

namespace hog::server {
namespace {

// What the protocol can carry of a wave height, metres.
constexpr float most_sea_height = 20.0F;

[[nodiscard]] double seconds(GameDuration duration) {
    return std::chrono::duration<double>(duration).count();
}

// The sea a wind raises over open water: grown full, until fetch and duration are modelled.
[[nodiscard]] protocol::CellWaves wind_sea(const weather::Column& column) {
    const auto speed = static_cast<double>(glm::length(column.wind));
    if (!column.sea || speed <= 0.0) {
        return {};
    }
    const waves::System grown = waves::grown_sea({column.wind.x, column.wind.y, 0.0});
    return {.height = std::min(static_cast<float>(grown.height), most_sea_height),
            .peak = static_cast<float>(grown.peak),
            .heading = column.wind / static_cast<float>(speed)};
}

[[nodiscard]] protocol::CellWeather shown(const weather::Column& column) {
    return {.cloud = column.cloud,
            .cover = column.cover,
            .falling = column.falling,
            .snow = column.snow,
            .drift = column.drift,
            .wind = column.wind,
            .humidity = column.humidity,
            .fog_top = column.fog_top,
            .base = column.base,
            .top = column.top,
            .wind_sea = wind_sea(column)};
}

} // namespace

WeatherKeeper::WeatherKeeper(const globe::Generator& generator, GameDuration day, double tilt,
                             GameTime start, WeatherSettings settings)
    : settings_(settings), calendar_(day, tilt), grid_(settings.level),
      air_(generator, seconds(day), settings.level, settings.layers),
      reached_(start - settings.spin_up) {
    placed_.resize(air_.field().grid.columns());
    for (std::size_t column = 0; column < placed_.size(); ++column) {
        const DVec3 up =
            globe::center(globe::cell_at_index(settings.level, static_cast<std::uint64_t>(column)));
        placed_[column] = grid_.index(planet::cell_at(up, settings.level));
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
        keep();
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

void WeatherKeeper::keep() {
    ByteWriter out;
    out.write(static_cast<std::uint64_t>(reached_.time_since_epoch().count()));
    out.write_bytes(weather::encode_state(air_.field()));
    saved_ = std::move(out).take();
}

Result<void> WeatherKeeper::restore(std::span<const std::byte> bytes, GameTime now) {
    ByteReader in{bytes};
    const auto micros = in.read<std::uint64_t>();
    if (!micros) {
        return std::unexpected(micros.error());
    }
    auto air = weather::decode_state(air_.field(), bytes.subspan(sizeof(std::uint64_t)));
    if (!air) {
        return air;
    }
    const GameTime reached{GameDuration{static_cast<std::int64_t>(*micros)}};
    // Air saved too long ago has lost what it knew; air saved ahead of the clock waits for it.
    reached_ = std::clamp(reached, now - settings_.spin_up, now);
    keep();
    if (now - reached_ < settings_.interval) {
        latest_ = sum(std::chrono::milliseconds{0});
    }
    return {};
}

std::optional<protocol::CellWeather> WeatherKeeper::at(DVec3 up) const {
    if (!latest_) {
        return std::nullopt;
    }
    return latest_->cells[grid_.index(planet::cell_at(up, settings_.level))];
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
