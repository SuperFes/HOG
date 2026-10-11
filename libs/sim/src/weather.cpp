#include <hog/core/assert.hpp>
#include <hog/core/bytes.hpp>
#include <hog/jobs/job_system.hpp>
#include <hog/planet/noise.hpp>
#include <hog/sim/weather.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <format>
#include <numbers>
#include <utility>

namespace hog::sim {

namespace {

constexpr double degree = std::numbers::pi / 180.0;
constexpr double hour = 3'600.0;
constexpr double day = 24.0 * hour;

// Gradients are taken as if over Earth.
constexpr double earth_radius = 6'371'000.0;
constexpr double air_density = 1.2;    // kg/m³
constexpr double gas_constant = 287.0; // dry air, J/(kg·K)
constexpr double lapse_rate = 0.0065;  // °C per metre
constexpr double sea_level_pressure = 1'013.0;

// Surface air over land follows the sun within hours; over the ocean it follows the sea.
constexpr double land_response = 6.0 * hour;
constexpr double ocean_response = 20.0 * day;
constexpr double upper_response = 4.0 * day;
constexpr double land_seasons = 55.0; // °C per unit of sin(declination) · sin(latitude)
constexpr double ocean_seasons = 15.0;
constexpr double land_days = 12.0; // °C per unit of sin(sun elevation)
constexpr double ocean_days = 0.6;
constexpr double cloud_shade = 0.6;

// The upper air cools across a band whose middle follows the sun; the jet runs along it.
constexpr double upper_equator = -6.0;
constexpr double upper_drop = 32.0;
constexpr double upper_drop_seasons = 15.0;
constexpr double band_latitude = 45.0 * degree;
constexpr double band_seasons = 15.0 * degree;
constexpr double band_width = 15.0 * degree;

// Bands of high and low pressure at Earth's: low at the equator, high near 30°, low near 60°.
constexpr double band_pressure = 8.0;    // hPa
constexpr double thermal_pressure = 0.5; // hPa per °C of surface warmth over the upper air
constexpr double ocean_friction = 4e-5;  // 1/s
constexpr double land_friction = 8e-5;
constexpr double max_wind = 50.0;
constexpr double max_jet = 90.0;
// The Coriolis parameter is held away from zero, where the thermal wind is undefined.
constexpr double min_coriolis_latitude = 10.0 * degree;

// Water, in mm of column depth.
constexpr double evaporation_rate = 0.25 / day; // of the shortfall from saturation
constexpr double condensing_humidity = 0.9;
constexpr double cloud_clearing = 3.0 * hour;
constexpr double raining_cloud = 0.4;
constexpr double rain_out = 2.0 * hour;
constexpr double cloud_cover_depth = 0.5;

// Storms grow where warm, moist surface air sits under cold upper air. The surface's moisture
// counts as warmth it releases on rising: a millimetre of column water is roughly a degree.
constexpr double neutral_contrast = 75.0; // °C
constexpr double storm_contrast = 10.0;
constexpr double storm_response = 1.0 * hour;
constexpr double storm_rain_out = 0.3;
constexpr double storm_anvil = 0.2;
constexpr double storm_mixing = 0.3;
// Most of a storm's heat is released aloft; the sea holds the air above it where it is.
constexpr double storm_land_cooling = 0.2;
constexpr double hail_storm = 0.5;
constexpr double max_hail = 0.6;
constexpr double hail_below = -15.0; // °C in the upper air
constexpr double snow_below = 1.5;   // °C, all snow 3 °C lower

constexpr std::uint32_t cells_per_job = 256;

// Column water the air holds at saturation, rising about 6% per °C.
double saturation(double temperature) {
    return 50.0 * std::exp(0.06 * (temperature - 25.0));
}

// Fraction of the way a value relaxing with time constant `response` moves in `step` seconds.
double approach(double step, double response) {
    return 1.0 - std::exp(-step / response);
}

double declination(GameTime time, double axial_tilt) {
    return std::asin(std::sin(axial_tilt * degree) * std::sin(Calendar::solar_longitude(time)));
}

double seconds(GameDuration duration) {
    return std::chrono::duration<double>(duration).count();
}

// Where the weather would settle at a latitude and season, before the time of day.
struct Targets {
    double surface = 0.0;
    double upper = 0.0;
    // The surface the upper air expects: the cell's mean with the ocean's seasons.
    double reference = 0.0;
};

Targets targets(const Ground& ground, double sin_latitude, double sin_declination) {
    const double seasons = sin_declination * sin_latitude;
    const double latitude = std::abs(std::asin(sin_latitude));
    const double summer = sin_latitude < 0.0 ? -sin_declination : sin_declination;
    const double middle = band_latitude + (band_seasons * summer);
    const double drop = upper_drop - (upper_drop_seasons * summer);
    const double cooling = 0.5 + (0.5 * std::tanh((latitude - middle) / band_width));
    return Targets{
        .surface = double{ground.mean_temperature} +
                   ((ground.ocean ? ocean_seasons : land_seasons) * seasons),
        .upper = upper_equator - (drop * cooling),
        .reference = double{ground.mean_temperature} + (ocean_seasons * seasons),
    };
}

double sea_level(double temperature, const Ground& ground) {
    return temperature + (lapse_rate * double{ground.elevation});
}

template <typename F>
void for_cells(jobs::JobSystem* jobs, std::uint32_t count, F body) {
    if (jobs == nullptr) {
        for (std::uint32_t index = 0; index < count; ++index) {
            body(index);
        }
        return;
    }
    jobs->parallel_for(count, cells_per_job, [&](jobs::Range range, std::uint32_t /*thread*/) {
        for (std::uint32_t index = range.begin; index < range.end; ++index) {
            body(index);
        }
    });
}

// Sea ice holds its water in.
float sea_wetness(double temperature) {
    constexpr double freezing = -2.0;
    return temperature < freezing ? 0.2F : 1.0F;
}

GameTime last_step(GameTime now, GameDuration step) {
    const auto since = now.time_since_epoch();
    auto steps = since / step;
    if (since % step < GameDuration::zero()) {
        --steps;
    }
    return GameTime{steps * step};
}

} // namespace

WeatherFields::WeatherFields(std::size_t size)
    : temperature(size), upper_temperature(size), pressure(size), vapour(size), cloud(size),
      wind(size, Vec2{0.0F}), jet(size, Vec2{0.0F}), evaporation(size), rain(size), snow(size),
      hail(size), storm(size) {}

float WeatherFields::cloud_cover(std::size_t index) const {
    return 1.0F - std::exp(-cloud[index] / static_cast<float>(cloud_cover_depth));
}

std::vector<Ground> ground_for(const CellGrid& grid, const planet::World& world) {
    const planet::Climate& climate = world.geology().climate();
    std::vector<Ground> ground(grid.size());
    for (std::uint32_t index = 0; index < grid.size(); ++index) {
        const planet::CellId cell = grid.cell(index);
        const planet::CellState state = world.state(cell);
        const bool ocean = state.biome == planet::Biome::Ocean;
        const double temperature = planet::fixed_to_double(climate.temperature(cell));
        ground[index] = Ground{
            .mean_temperature = static_cast<float>(temperature),
            .elevation =
                ocean ? 0.0F
                      : static_cast<float>(std::max(planet::fixed_to_double(state.elevation), 0.0)),
            .wetness = ocean ? sea_wetness(temperature)
                             : static_cast<float>(planet::fixed_to_double(climate.moisture(cell))),
            .ocean = ocean,
        };
    }
    return ground;
}

Weather::Weather(const WeatherConfig& config, std::vector<Ground> ground, GameTime now)
    : config_(config), grid_(config.level), ground_(std::move(ground)),
      calendar_(config.day > GameDuration::zero() ? config.day : GameDuration{1},
                config.axial_tilt),
      fields_(grid_.size()),
      time_(last_step(now, config.step > GameDuration::zero() ? config.step : GameDuration{1})) {
    HOG_ASSERT(config.step > GameDuration::zero(), "weather step must be positive");
    HOG_ASSERT(config.day > GameDuration::zero(), "weather day must be positive");
    HOG_ASSERT(ground_.size() == grid_.size(), "need ground for every cell");
    const double sin_declination = std::sin(declination(time_, config_.axial_tilt));
    for (std::uint32_t index = 0; index < grid_.size(); ++index) {
        const Targets target = targets(ground_[index], grid_.frame(index).up.z, sin_declination);
        fields_.temperature[index] = static_cast<float>(target.surface);
        fields_.upper_temperature[index] = static_cast<float>(target.upper);
        fields_.vapour[index] =
            static_cast<float>(0.7 * saturation(target.surface) * double{ground_[index].wetness});
    }
    balance_winds(nullptr);
}

std::uint64_t Weather::advance_to(GameTime now, jobs::JobSystem& jobs) {
    std::uint64_t steps = 0;
    while (time_ + config_.step <= now) {
        step(time_ + config_.step, jobs);
        ++steps;
    }
    return steps;
}

void Weather::step(GameTime at, jobs::JobSystem& jobs) {
    const double dt = seconds(config_.step);
    const GameTime middle = at - (config_.step / 2);
    const double sin_declination = std::sin(declination(middle, config_.axial_tilt));
    const double land_move = approach(dt, land_response);
    const double ocean_move = approach(dt, ocean_response);
    const double upper_move = approach(dt, upper_response);
    const double storm_move = approach(dt, storm_response);
    const double clearing = approach(dt, cloud_clearing);
    const double raining = approach(dt, rain_out);
    const double evaporating = std::min(evaporation_rate * dt, 1.0);

    for_cells(&jobs, grid_.size(), [&](std::uint32_t index) {
        const Ground& ground = ground_[index];
        const DVec3 up = grid_.frame(index).up;
        WeatherFields& f = fields_;
        const Targets target = targets(ground, up.z, sin_declination);
        const auto cover = double{f.cloud_cover(index)};
        const double sun = std::sin(calendar_.sun_elevation(middle, up));
        const double heating =
            (ground.ocean ? ocean_days : land_days) * sun * (1.0 - (cloud_shade * cover));

        auto surface = double{f.temperature[index]};
        auto upper = double{f.upper_temperature[index]};
        surface += (target.surface + heating - surface) * (ground.ocean ? ocean_move : land_move);
        upper += (target.upper - upper) * upper_move;

        auto vapour = double{f.vapour[index]};
        auto cloud = double{f.cloud[index]};
        const double holds = saturation(surface);
        const double evaporation =
            std::max(holds - vapour, 0.0) * double{ground.wetness} * evaporating;
        vapour += evaporation;

        // Thunderstorms rain straight out of the air and stir the column towards neutral.
        const double contrast = sea_level(surface, ground) + vapour - upper - neutral_contrast;
        const double storm = std::clamp(contrast / storm_contrast, 0.0, 1.0);
        const double convected = storm * vapour * storm_rain_out * storm_move;
        vapour -= convected;
        cloud += convected * storm_anvil;
        const double mixed = storm * std::max(contrast, 0.0) * storm_mixing * storm_move;
        surface -= ground.ocean ? 0.0 : mixed * storm_land_cooling;
        upper += mixed;

        const double limit = condensing_humidity * holds;
        if (vapour > limit) {
            cloud += vapour - limit;
            vapour = limit;
        } else {
            const double cleared = std::min(cloud, limit - vapour) * clearing;
            cloud -= cleared;
            vapour += cleared;
        }
        const double stratiform = std::max(cloud - raining_cloud, 0.0) * raining;
        cloud -= stratiform;

        const double storm_fall = convected * (1.0 - storm_anvil);
        const double hail = upper < hail_below
                                ? storm_fall * std::clamp((storm - hail_storm) * 1.2, 0.0, max_hail)
                                : 0.0;
        const double falling = stratiform + storm_fall - hail;
        const double snowy = std::clamp((snow_below - surface) / 3.0, 0.0, 1.0);

        f.temperature[index] = static_cast<float>(surface);
        f.upper_temperature[index] = static_cast<float>(upper);
        f.vapour[index] = static_cast<float>(vapour);
        f.cloud[index] = static_cast<float>(cloud);
        f.evaporation[index] = static_cast<float>(evaporation);
        f.rain[index] = static_cast<float>(falling * (1.0 - snowy));
        f.snow[index] = static_cast<float>(falling * snowy);
        f.hail[index] = static_cast<float>(hail);
        f.storm[index] = static_cast<float>(storm);
    });
    time_ = at;
    balance_winds(&jobs);
}

void Weather::balance_winds(jobs::JobSystem* jobs) {
    const double sin_declination = std::sin(declination(time_, config_.axial_tilt));
    for_cells(jobs, grid_.size(), [&](std::uint32_t index) {
        const Ground& ground = ground_[index];
        const double sin_latitude = grid_.frame(index).up.z;
        const double latitude = std::asin(sin_latitude);
        const Targets target = targets(ground, sin_latitude, sin_declination);
        const double warmth = (sea_level(double{fields_.temperature[index]}, ground) -
                               double{fields_.upper_temperature[index]}) -
                              (target.reference - target.upper);
        fields_.pressure[index] =
            static_cast<float>(sea_level_pressure - (band_pressure * std::cos(6.0 * latitude)) -
                               (thermal_pressure * warmth));
    });

    const double rotation = 2.0 * std::numbers::pi / seconds(config_.day);
    for_cells(jobs, grid_.size(), [&](std::uint32_t index) {
        const auto& around = grid_.neighbors(index);
        const auto& weights = grid_.gradient_weights(index);
        DVec2 pressure_gradient{0.0};
        DVec2 upper_gradient{0.0};
        for (std::size_t k = 0; k < around.size(); ++k) {
            pressure_gradient +=
                weights[k] * double{fields_.pressure[around[k]] - fields_.pressure[index]};
            upper_gradient += weights[k] * double{fields_.upper_temperature[around[k]] -
                                                  fields_.upper_temperature[index]};
        }
        // Per metre over Earth, in Pa and °C.
        pressure_gradient *= 100.0 / earth_radius;
        upper_gradient /= earth_radius;

        const double sin_latitude = grid_.frame(index).up.z;
        const double coriolis = 2.0 * rotation * sin_latitude;
        const double friction = ground_[index].ocean ? ocean_friction : land_friction;
        // Steady balance of the pressure force, the Coriolis force and friction.
        const DVec2 push = -pressure_gradient / air_density;
        const DVec2 turned{-push.y, push.x};
        DVec2 wind = ((friction * push) - (coriolis * turned)) /
                     ((friction * friction) + (coriolis * coriolis));
        if (glm::length(wind) > max_wind) {
            wind *= max_wind / glm::length(wind);
        }

        // The thermal wind: the jet blows with cold air on its left in the north, on its right
        // in the south.
        const double floor = 2.0 * rotation * std::sin(min_coriolis_latitude);
        const double held =
            std::abs(coriolis) < floor ? std::copysign(floor, sin_latitude) : coriolis;
        const DVec2 shear =
            gas_constant * std::numbers::ln2 / held * DVec2{-upper_gradient.y, upper_gradient.x};
        DVec2 jet = wind + shear;
        if (glm::length(jet) > max_jet) {
            jet *= max_jet / glm::length(jet);
        }
        fields_.wind[index] = Vec2{wind};
        fields_.jet[index] = Vec2{jet};
    });
}

std::vector<std::byte> Weather::save() const {
    ByteWriter out;
    out.write(static_cast<std::uint8_t>(grid_.level()));
    out.write(static_cast<std::uint64_t>(time_.time_since_epoch().count()));
    const auto floats = [&](const std::vector<float>& values) {
        for (const float value : values) {
            out.write(std::bit_cast<std::uint32_t>(value));
        }
    };
    const auto vectors = [&](const std::vector<Vec2>& values) {
        for (const Vec2 value : values) {
            out.write(std::bit_cast<std::uint32_t>(value.x));
            out.write(std::bit_cast<std::uint32_t>(value.y));
        }
    };
    floats(fields_.temperature);
    floats(fields_.upper_temperature);
    floats(fields_.pressure);
    floats(fields_.vapour);
    floats(fields_.cloud);
    vectors(fields_.wind);
    vectors(fields_.jet);
    floats(fields_.evaporation);
    floats(fields_.rain);
    floats(fields_.snow);
    floats(fields_.hail);
    floats(fields_.storm);
    return std::move(out).take();
}

Result<void> Weather::load(std::uint16_t version, std::span<const std::byte> bytes) {
    if (version != save_version) {
        return make_error(ErrorCode::Unsupported,
                          std::format("weather version {} is newer than this build", version));
    }
    ByteReader in{bytes};
    const auto level = in.read<std::uint8_t>();
    if (!level) {
        return std::unexpected(level.error());
    }
    if (*level != grid_.level()) {
        return make_error(
            ErrorCode::InvalidArgument,
            std::format("weather was saved at level {}, not {}", *level, grid_.level()));
    }
    const auto time = in.read<std::uint64_t>();
    if (!time) {
        return std::unexpected(time.error());
    }
    WeatherFields loaded{grid_.size()};
    Result<void> read{};
    const auto next = [&] {
        auto bits = read ? in.read<std::uint32_t>() : Result<std::uint32_t>{0U};
        if (!bits) {
            read = std::unexpected(std::move(bits.error()));
        }
        return std::bit_cast<float>(bits.value_or(0U));
    };
    const auto floats = [&](std::vector<float>& values) {
        for (float& value : values) {
            value = next();
        }
    };
    const auto vectors = [&](std::vector<Vec2>& values) {
        for (Vec2& value : values) {
            value.x = next();
            value.y = next();
        }
    };
    floats(loaded.temperature);
    floats(loaded.upper_temperature);
    floats(loaded.pressure);
    floats(loaded.vapour);
    floats(loaded.cloud);
    vectors(loaded.wind);
    vectors(loaded.jet);
    floats(loaded.evaporation);
    floats(loaded.rain);
    floats(loaded.snow);
    floats(loaded.hail);
    floats(loaded.storm);
    if (!read) {
        return read;
    }
    if (auto finished = in.finish(); !finished) {
        return finished;
    }
    fields_ = std::move(loaded);
    time_ = GameTime{GameDuration{static_cast<GameDuration::rep>(*time)}};
    return {};
}

} // namespace hog::sim
