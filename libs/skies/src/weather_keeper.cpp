#include <hog/core/bytes.hpp>
#include <hog/globe/cell.hpp>
#include <hog/planet/projection.hpp>
#include <hog/sim/cell_grid.hpp>
#include <hog/skies/weather_keeper.hpp>
#include <hog/waves/waves.hpp>
#include <hog/weather/state.hpp>

#include <algorithm>
#include <cmath>
#include <numbers>
#include <utility>

namespace hog::skies {
namespace {

// The longest a weather is faded to over, so a slow job doesn't smear the cloud.
constexpr std::chrono::milliseconds most_fade{60'000};

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
            .base = column.base,
            .top = column.top,
            .wind_sea = wind_sea(column)};
}

} // namespace

int weather_level(double radius) noexcept {
    // A face is a quarter of the way round; its cells halve across with each level.
    const double across = radius * std::numbers::pi / 2.0 / weather_column_width;
    const auto level = static_cast<int>(std::lround(std::log2(std::max(across, 1.0))));
    return std::clamp(level, 1, static_cast<int>(protocol::most_weather_level));
}

WeatherKeeper::WeatherKeeper(const globe::Generator& generator, GameTime start,
                             WeatherSettings settings)
    : settings_(settings),
      level_(settings.level.value_or(weather_level(generator.planet().body.radius))),
      calendar_(globe::day_length(generator.planet()), generator.planet().body.tilt), grid_(level_),
      air_(generator, level_, settings.layers), reached_(start - settings.spin_up) {
    placed_.resize(air_.field().grid.columns());
    for (std::size_t column = 0; column < placed_.size(); ++column) {
        const DVec3 up =
            globe::center(globe::cell_at_index(level_, static_cast<std::uint64_t>(column)));
        placed_[column] = grid_.index(planet::cell_at(up, level_));
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
        ran_ += running_;
        // Until the air has caught up with the clock, or run its spin-up, there is nothing true
        // to show; after, it's shown from every job, however far behind the clock it runs.
        live_ = live_ || now - reached_ < settings_.interval || ran_ >= settings_.spin_up;
        if (live_) {
            const auto real = std::chrono::steady_clock::now();
            // The weather is as old as the real time since the last, and carried at the game time
            // it ran per real second.
            float pace = 1.0F;
            if (last_made_ != std::chrono::steady_clock::time_point{}) {
                const std::chrono::duration<double> since = real - last_made_;
                const auto apart = std::chrono::duration_cast<std::chrono::milliseconds>(since);
                // Never less than the fade asked for, nor more than the most unless that is more.
                fade = std::min(std::max(fade, apart), std::max(fade, most_fade));
                pace = static_cast<float>(seconds(running_) / std::max(since.count(), 0.5));
            }
            last_made_ = real;
            made = sum(fade, pace);
            latest_ = made;
        }
    }
    // Hurried past what the air can be stepped at, it skips ahead rather than fall further behind.
    if (live_ && now - reached_ > settings_.lag) {
        reached_ = now - settings_.lag;
    }
    if (now - reached_ < settings_.interval) {
        return made;
    }
    const GameDuration run = std::min(now - reached_, settings_.longest);
    const GameTime from = reached_;
    reached_ += run;
    running_ = run;
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
    // Air that kept its winds is shown as it is; air that lost them has its spin-up to run.
    live_ = reached >= now - settings_.spin_up;
    if (live_) {
        latest_ = sum(std::chrono::milliseconds{0}, 1.0F);
    }
    return {};
}

std::optional<protocol::CellWeather> WeatherKeeper::at(DVec3 up) const {
    if (!latest_) {
        return std::nullopt;
    }
    return latest_->cells[grid_.index(planet::cell_at(up, level_))];
}

protocol::Weather WeatherKeeper::sum(std::chrono::milliseconds fade, float pace) const {
    protocol::Weather weather{.level = static_cast<std::uint8_t>(level_),
                              .fade = fade,
                              .pace = pace,
                              .cells = std::vector<protocol::CellWeather>(placed_.size())};
    for (std::size_t column = 0; column < placed_.size(); ++column) {
        weather.cells[placed_[column]] = shown(air_.column(column));
    }
    return weather;
}

} // namespace hog::skies
