#pragma once

#include <hog/core/time.hpp>
#include <hog/globe/generator.hpp>
#include <hog/protocol/messages.hpp>
#include <hog/sim/calendar.hpp>
#include <hog/weather/atmosphere.hpp>

#include <chrono>
#include <cstdint>
#include <future>
#include <optional>
#include <vector>

namespace hog::server {

// How fine the air is held and how often it is stepped.
struct WeatherSettings {
    int level = 4;
    int layers = 24;
    // Game time the air waits to be stepped on by, so each job is worth its start.
    GameDuration interval = std::chrono::minutes{2};
    // Game time the most one job runs on, so a hurried clock does not hold a job for long.
    GameDuration longest = std::chrono::hours{1};
    // Game time a new world's still air is run on before it is shown, for its winds to build.
    GameDuration spin_up = std::chrono::hours{48};
};

// Steps the planet's air in the background as game time passes and sums it for the players.
class WeatherKeeper {
public:
    WeatherKeeper(const globe::Generator& generator, GameDuration day, double tilt, GameTime start,
                  WeatherSettings settings = {});
    ~WeatherKeeper();
    WeatherKeeper(const WeatherKeeper&) = delete;
    WeatherKeeper& operator=(const WeatherKeeper&) = delete;
    WeatherKeeper(WeatherKeeper&&) = delete;
    WeatherKeeper& operator=(WeatherKeeper&&) = delete;

    // Takes in what a finished job did and starts the next if the game time reached has outrun
    // the air by an interval. Returns the weather it just made, if it made any; the client fades to
    // it over `fade`.
    [[nodiscard]] std::optional<protocol::Weather> poll(GameTime now,
                                                        std::chrono::milliseconds fade);

    // The newest weather made, for those joining.
    [[nodiscard]] const std::optional<protocol::Weather>& latest() const noexcept {
        return latest_;
    }

private:
    [[nodiscard]] protocol::Weather sum(std::chrono::milliseconds fade) const;

    WeatherSettings settings_;
    sim::Calendar calendar_;
    weather::Atmosphere air_;
    // Where each air column's cell is in the message, and the other way round.
    std::vector<std::uint32_t> placed_;
    // How far game time the air has run.
    GameTime reached_;
    std::future<void> job_;
    std::optional<protocol::Weather> latest_;
};

} // namespace hog::server
