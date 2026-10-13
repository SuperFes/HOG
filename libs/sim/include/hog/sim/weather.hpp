#pragma once

#include <hog/core/math.hpp>
#include <hog/core/result.hpp>
#include <hog/core/time.hpp>
#include <hog/planet/world.hpp>
#include <hog/sim/calendar.hpp>
#include <hog/sim/cell_grid.hpp>
#include <hog/sim/land.hpp>
#include <hog/sim/ocean.hpp>
#include <hog/sim/weather_tuning.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace hog::jobs {
class JobSystem;
}

namespace hog::sim {

struct WeatherConfig {
    int level = 5;
    // Steps fall on whole multiples of this since the epoch, whatever the tick rate.
    GameDuration step = std::chrono::minutes{10};
    // The day the weather's heating and the Coriolis force follow, which needn't be the sun's.
    GameDuration day = std::chrono::hours{24};
    double axial_tilt = 23.44;        // degrees
    double planet_radius = 115'000.0; // metres, for how steep the ground is
    std::uint64_t seed = 0;           // where and when storm systems form
    WeatherTuning tuning{};
};

// What a cell's weather sits on.
struct Ground {
    float mean_temperature = 15.0F; // annual mean at the surface, °C
    float elevation = 0.0F;         // metres, 0 over the ocean
    float wetness = 1.0F;           // how freely the surface gives up water, 0 to 1
    bool ocean = true;
};

// The ground under every grid cell, from the planet's terrain, changes and climate.
[[nodiscard]] std::vector<Ground> ground_for(const CellGrid& grid, const planet::World& world);

// Two layers of air per cell: the surface and the upper air near 500 hPa. Winds are (east, north)
// metres per second; water is millimetres of column depth, so it adds up across fields. The
// planet is small, so distances are scaled up to an Earth-sized sphere: systems cross it at the
// pace Earth's would, and winds have Earth-like speeds.
struct WeatherFields {
    std::vector<float> temperature;       // surface air, °C
    std::vector<float> upper_temperature; // °C
    std::vector<float> pressure;          // at sea level, hPa
    std::vector<float> vapour;            // mm
    std::vector<float> cloud;             // mm of condensed water
    std::vector<Vec2> wind;               // at the surface
    std::vector<Vec2> jet;                // in the upper air
    // What the last step did, in mm.
    std::vector<float> evaporation;
    std::vector<float> rain;
    std::vector<float> snow;
    std::vector<float> hail;
    // Thunderstorm strength over the last step, 0 to 1.
    std::vector<float> storm;
    // How fast the air is rising, in m/s; negative where it sinks.
    std::vector<float> lift;
    // Fronts, from -1 to 1: positive where warmer air is arriving, negative where colder is.
    std::vector<float> front;
    // Running means: surface °C, and rain, snow and hail together in mm per day.
    std::vector<float> recent_temperature;
    std::vector<float> recent_precipitation;

    explicit WeatherFields(std::size_t size = 0);

    [[nodiscard]] std::size_t size() const noexcept { return temperature.size(); }
    // Fraction of the sky covered, 0 to 1.
    [[nodiscard]] float cloud_cover(std::size_t index) const;

    friend bool operator==(const WeatherFields&, const WeatherFields&) = default;
};

// Fraction of the sky covered, 0 to 1, by mm of cloud.
[[nodiscard]] float cloud_cover(float cloud) noexcept;

// A travelling low.
struct StormSystem {
    std::uint32_t id = 0;
    DVec3 centre{1.0, 0.0, 0.0}; // unit direction from the planet's centre
    float depth = 0.0F;          // hPa at its deepest
    float radius = 0.0F;         // radians
    float age = 0.0F;            // seconds
    float life = 0.0F;           // seconds

    // hPa below the surroundings now: deepening for the first `growing` share of its life, then
    // filling.
    [[nodiscard]] double strength(double growing) const;

    friend bool operator==(const StormSystem&, const StormSystem&) = default;
};

class Weather {
public:
    // Starts from the weather the season and time of day would bring, as of the last step at or
    // before `now`. Precondition: one ground per cell at the configured level, positive step
    // and day.
    Weather(const WeatherConfig& config, std::vector<Ground> ground, GameTime now);

    [[nodiscard]] const WeatherConfig& config() const noexcept { return config_; }
    [[nodiscard]] const CellGrid& grid() const noexcept { return grid_; }
    [[nodiscard]] const std::vector<Ground>& ground() const noexcept { return ground_; }
    [[nodiscard]] const WeatherFields& fields() const noexcept { return fields_; }
    [[nodiscard]] const std::vector<StormSystem>& systems() const noexcept { return systems_; }
    [[nodiscard]] const OceanFields& ocean() const noexcept { return ocean_.fields(); }
    [[nodiscard]] const Land& land() const noexcept { return land_; }
    // Replaces the weather as it stands, for tools and tests. Precondition: one value per cell.
    void set_fields(WeatherFields fields);
    // When the last step was taken.
    [[nodiscard]] GameTime time() const noexcept { return time_; }

    // Takes every step due by `now`; returns how many. Call from the jobs' owning thread.
    std::uint64_t advance_to(GameTime now, jobs::JobSystem& jobs);

    // Versioned bytes for a snapshot section.
    [[nodiscard]] std::vector<std::byte> save() const;
    // Replaces the state with saved bytes, or fails and changes nothing: with Unsupported for a
    // newer version, and InvalidArgument for weather saved at another grid level, which can't be
    // carried over.
    [[nodiscard]] Result<void> load(std::uint16_t version, std::span<const std::byte> bytes);

    static constexpr std::uint16_t save_version = 5;

private:
    // The air as the season and the sea would leave it, with its winds balanced.
    void start_air();
    // Settles the sea's currents under the winds as they stand, then warms it through the months
    // before the last step, so it starts with its seasons late and its currents' warmth carried.
    void spin_up_ocean();
    // Runs the land through the years before the last step under the seasons' usual weather, so
    // it starts with its snow, soil and rivers as the season leaves them.
    void spin_up_land();
    void step(GameTime at, jobs::JobSystem& jobs);
    // Pressure follows temperature; winds follow pressure and the upper air's temperature.
    void balance_winds(jobs::JobSystem* jobs);

    WeatherConfig config_;
    CellGrid grid_;
    std::vector<Ground> ground_;
    // How the ground rises per metre east and north.
    std::vector<DVec2> slopes_;
    Calendar calendar_;
    WeatherFields fields_;
    // The fields as the step began, which moving reads from.
    WeatherFields before_;
    std::vector<StormSystem> systems_;
    std::uint32_t next_system_ = 1;
    GameTime time_;
    Ocean ocean_;
    Land land_;
};

} // namespace hog::sim
