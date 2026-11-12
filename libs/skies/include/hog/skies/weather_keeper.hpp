#pragma once

#include <hog/core/result.hpp>
#include <hog/core/time.hpp>
#include <hog/globe/generator.hpp>
#include <hog/protocol/messages.hpp>
#include <hog/save/snapshot.hpp>
#include <hog/sim/calendar.hpp>
#include <hog/sim/cell_grid.hpp>
#include <hog/weather/atmosphere.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <future>
#include <optional>
#include <span>
#include <vector>

namespace hog::skies {

// Metres across the air's columns are held about, on a planet small enough for them.
inline constexpr double weather_column_width = 500.0;

// The cube-sphere level whose cells are nearest `weather_column_width` across on a planet of
// `radius` metres, no finer than the weather is sent at.
[[nodiscard]] int weather_level(double radius) noexcept;

// How fine the air is held and how often it is stepped.
struct WeatherSettings {
    // As weather_level() has it for the planet where none's given.
    std::optional<int> level = std::nullopt;
    int layers = 24;
    // Game time the air waits to be stepped on by, so each job is worth its start.
    GameDuration interval = std::chrono::minutes{2};
    // Game time the most one job runs on, so a hurried clock does not hold a job for long.
    GameDuration longest = std::chrono::hours{1};
    // Game time a new world's still air is run on before it is shown, for its winds to build.
    GameDuration spin_up = std::chrono::hours{48};
    // Game time the air may fall behind the clock once it is shown; when the clock runs faster
    // than the air can be stepped, the air skips ahead rather than fall further.
    GameDuration lag = std::chrono::hours{6};
};

// Steps the planet's air in the background as game time passes and sums it for the players.
class WeatherKeeper {
public:
    // The snapshot section the air is kept in, and the version of the bytes in it.
    static constexpr save::SectionId section = save::section_id("AIRS");
    static constexpr std::uint16_t section_version = 1;

    WeatherKeeper(const globe::Generator& generator, GameTime start, WeatherSettings settings = {});
    ~WeatherKeeper();
    WeatherKeeper(const WeatherKeeper&) = delete;
    WeatherKeeper& operator=(const WeatherKeeper&) = delete;
    WeatherKeeper(WeatherKeeper&&) = delete;
    WeatherKeeper& operator=(WeatherKeeper&&) = delete;

    // Takes in what a finished job did and starts the next if the game time reached has outrun
    // the air by an interval. Returns the weather it just made, if it made any; the client fades to
    // it over `fade`, or over the real time since the last if that was longer. Shown from the
    // moment the air has caught up with the clock or run its spin-up, then after every job, however
    // far behind the clock the air runs.
    [[nodiscard]] std::optional<protocol::Weather> poll(GameTime now,
                                                        std::chrono::milliseconds fade);

    // The air as of the last job that finished, written for a save: the game time it had reached
    // and what the air held. Empty until a job has finished or a save has been restored.
    [[nodiscard]] const std::vector<std::byte>& saved() const noexcept { return saved_; }

    // Takes up the air a save held, if it is for this grid, and carries on from where it had got
    // to; a save further behind `now` than the spin-up is as good as a new world's still air.
    // Call before the first poll.
    [[nodiscard]] Result<void> restore(std::span<const std::byte> bytes, GameTime now);

    // The weather over the planet's point in the direction `up` (unit, from the centre) as of
    // the newest made; none until some has been.
    [[nodiscard]] std::optional<protocol::CellWeather> at(DVec3 up) const;

    // The newest weather made, for those joining.
    [[nodiscard]] const std::optional<protocol::Weather>& latest() const noexcept {
        return latest_;
    }

private:
    [[nodiscard]] protocol::Weather sum(std::chrono::milliseconds fade, float pace) const;
    void keep();

    WeatherSettings settings_;
    int level_;
    sim::Calendar calendar_;
    sim::CellGrid grid_;
    weather::Atmosphere air_;
    // Where each air column's cell is in the message, and the other way round.
    std::vector<std::uint32_t> placed_;
    // How far game time the air has run.
    GameTime reached_;
    std::future<void> job_;
    GameDuration running_{}; // game time the job in flight runs on
    GameDuration ran_{};     // game time the air has run since it started or was restored
    bool live_ = false;      // whether the air is shown
    std::chrono::steady_clock::time_point last_made_{}; // real time of the last weather made
    std::optional<protocol::Weather> latest_;
    std::vector<std::byte> saved_;
};

} // namespace hog::skies
