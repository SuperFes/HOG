#include <hog/client/forecast.hpp>
#include <hog/planet/tangent_frame.hpp>

#include <glm/glm.hpp>
#include <glm/gtc/packing.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
#include <utility>

namespace hog::client {

namespace {

// A fade takes at least this share of the time since the last weather, if that was a believable
// gap.
constexpr double fade_share = 0.95;
constexpr auto max_apart = std::chrono::hours{1};

} // namespace

namespace {

using Cells = std::vector<protocol::CellWeather>;

template <typename T>
T blend(T from, T to, float by) noexcept {
    return from + ((to - from) * by);
}

// Waves add by variance, so each system counts by its height's square.
protocol::CellWaves mixed(const protocol::CellWaves& from, const protocol::CellWaves& to,
                          float by) noexcept {
    const float before = from.peak > 0.0F ? (1.0F - by) * from.height * from.height : 0.0F;
    const float after = to.peak > 0.0F ? by * to.height * to.height : 0.0F;
    const float total = before + after;
    if (total <= 0.0F) {
        return {};
    }
    const float share = after / total;
    const Vec2 heading = blend(from.heading, to.heading, share);
    // Running opposite ways, as evenly, they run the stronger's way.
    const Vec2 stronger = share >= 0.5F ? to.heading : from.heading;
    return {.height = std::sqrt(total),
            .peak = blend(from.peak, to.peak, share),
            .heading = glm::length(heading) > 1e-6F ? glm::normalize(heading) : stronger};
}

protocol::CellWeather mixed(const protocol::CellWeather& from, const protocol::CellWeather& to,
                            float by) noexcept {
    return {.cloud = blend(from.cloud, to.cloud, by),
            .cover = blend(from.cover, to.cover, by),
            .falling = blend(from.falling, to.falling, by),
            .snow = blend(from.snow, to.snow, by),
            .drift = blend(from.drift, to.drift, by),
            .wind = blend(from.wind, to.wind, by),
            .humidity = blend(from.humidity, to.humidity, by),
            .fog_top = blend(from.fog_top, to.fog_top, by),
            .base = blend(from.base, to.base, by),
            .top = blend(from.top, to.top, by),
            .wind_sea = mixed(from.wind_sea, to.wind_sea, by),
            .swells = {mixed(from.swells[0], to.swells[0], by),
                       mixed(from.swells[1], to.swells[1], by)},
            .current = blend(from.current, to.current, by)};
}

protocol::CellWeather interpolated(const sim::CellGrid& grid, const Cells& cells, DVec3 direction) {
    const auto field = [&](auto part) {
        return static_cast<float>(
            grid.interpolate(direction, [&](std::uint32_t index) { return part(cells[index]); }));
    };
    return {.cloud = field([](const auto& cell) { return static_cast<double>(cell.cloud); }),
            .cover = field([](const auto& cell) { return static_cast<double>(cell.cover); }),
            .falling = field([](const auto& cell) { return static_cast<double>(cell.falling); }),
            .snow = field([](const auto& cell) { return static_cast<double>(cell.snow); }),
            .drift = {field([](const auto& cell) { return static_cast<double>(cell.drift.x); }),
                      field([](const auto& cell) { return static_cast<double>(cell.drift.y); })},
            .wind = {field([](const auto& cell) { return static_cast<double>(cell.wind.x); }),
                     field([](const auto& cell) { return static_cast<double>(cell.wind.y); })},
            .humidity = field([](const auto& cell) { return static_cast<double>(cell.humidity); }),
            .fog_top = field([](const auto& cell) { return static_cast<double>(cell.fog_top); }),
            .base = field([](const auto& cell) { return static_cast<double>(cell.base); }),
            .top = field([](const auto& cell) { return static_cast<double>(cell.top); })};
}

// Cells with no cloud aloft have no base or top, which blending a cloud's edge with them would
// pull to the ground as a tent: they take the heights the cloud keeps over the planet, as
// much as each cell has of it, so what lies between is cloud at those heights, only thinner.
void fill_heights(Cells& cells) {
    double base = 0.0;
    double top = 0.0;
    double weight = 0.0;
    for (const protocol::CellWeather& cell : cells) {
        if (cell.top > cell.base && cell.cloud > 0.0F) {
            const auto mass = static_cast<double>(cell.cloud);
            base += mass * static_cast<double>(cell.base);
            top += mass * static_cast<double>(cell.top);
            weight += mass;
        }
    }
    if (weight <= 0.0) {
        return;
    }
    for (protocol::CellWeather& cell : cells) {
        if (!(cell.top > cell.base) || !(cell.cloud > 0.0F)) {
            cell.base = static_cast<float>(base / weight);
            cell.top = static_cast<float>(top / weight);
        }
    }
}

std::uint8_t to_byte(double share) noexcept {
    return static_cast<std::uint8_t>(std::lround(std::clamp(share, 0.0, 1.0) * 255.0));
}

struct Maps {
    std::vector<WeatherTexel> weather;
    std::vector<SkyTexel> sky;
    std::vector<DriftTexel> drift;
};

Maps maps_of(const sim::CellGrid& grid, const Cells& cells) {
    constexpr std::uint32_t size = weather_map_size;
    Maps maps;
    maps.weather.reserve(std::size_t{6} * size * size);
    maps.sky.reserve(std::size_t{6} * size * size);
    maps.drift.reserve(std::size_t{6} * size * size);
    for (std::uint32_t face = 0; face < 6; ++face) {
        for (std::uint32_t y = 0; y < size; ++y) {
            for (std::uint32_t x = 0; x < size; ++x) {
                const DVec3 direction = cube_direction(face, x, y, size);
                const protocol::CellWeather cell = interpolated(grid, cells, direction);
                const Vec3 drift{planet::tangent_frame(direction).to_world(
                    {static_cast<double>(cell.drift.x), static_cast<double>(cell.drift.y), 0.0})};
                maps.drift.push_back({glm::packHalf1x16(drift.x), glm::packHalf1x16(drift.y),
                                      glm::packHalf1x16(drift.z), 0});
                maps.weather.push_back(
                    {to_byte(static_cast<double>(cell.cover)),
                     to_byte(std::sqrt(std::max(static_cast<double>(cell.cloud), 0.0) / 16.0)),
                     to_byte(std::sqrt(std::max(static_cast<double>(cell.falling), 0.0) / 256.0)),
                     to_byte(static_cast<double>(cell.snow))});
                maps.sky.push_back(
                    {to_byte(static_cast<double>(cell.humidity)),
                     to_byte(static_cast<double>(cell.fog_top) / (255.0 * fog_texel_step)),
                     to_byte(static_cast<double>(cell.base) / (255.0 * height_texel_step)),
                     to_byte(static_cast<double>(cell.top) / (255.0 * height_texel_step))});
            }
        }
    }
    return maps;
}

} // namespace

std::string describe(const protocol::CellWeather& weather) {
    const double cover = 100.0 * static_cast<double>(weather.cover);
    std::string line = std::format("{:.0f}% cloud", cover);
    if (cover >= 0.5) {
        line += std::format(" at {:.0f}-{:.0f} m", static_cast<double>(weather.base),
                            static_cast<double>(weather.top));
    }
    if (weather.falling > 0.0F) {
        const char* falls = weather.snow >= 0.5F ? "snow" : "rain";
        line += std::format(", {} {:.1f} mm/h", falls, static_cast<double>(weather.falling));
    }
    line += std::format(", {:.0f}% humid", 100.0 * static_cast<double>(weather.humidity));
    if (weather.fog_top > 0.0F) {
        line += std::format(", fog to {:.0f} m", static_cast<double>(weather.fog_top));
    }
    return line +
           std::format(", drifting {:.0f} m/s", static_cast<double>(glm::length(weather.drift)));
}

DVec3 cube_direction(std::uint32_t face, std::uint32_t x, std::uint32_t y,
                     std::uint32_t size) noexcept {
    const double s = ((2.0 * (x + 0.5)) / size) - 1.0;
    const double t = ((2.0 * (y + 0.5)) / size) - 1.0;
    // Vulkan's table of which axis each face looks along, and which way s and t run on it.
    switch (face) {
    case 0:
        return glm::normalize(DVec3{1.0, -t, -s});
    case 1:
        return glm::normalize(DVec3{-1.0, -t, s});
    case 2:
        return glm::normalize(DVec3{s, 1.0, t});
    case 3:
        return glm::normalize(DVec3{s, -1.0, -t});
    case 4:
        return glm::normalize(DVec3{s, -t, 1.0});
    default:
        return glm::normalize(DVec3{-s, -t, -1.0});
    }
}

namespace {

// The cloud is carried as fast as the weather ran, in game time, for each real second.
void carry_at_pace(std::vector<protocol::CellWeather>& cells, float pace) {
    for (protocol::CellWeather& cell : cells) {
        cell.drift *= pace;
    }
}

} // namespace

void Forecast::receive(const protocol::Weather& weather, MonotonicClock::time_point now) {
    if (!grid_ || grid_->level() != weather.level) {
        grid_.emplace(weather.level);
        to_ = weather.cells;
        carry_at_pace(to_, weather.pace);
        fill_heights(to_);
        from_ = to_;
        fade_ = {};
        waiting_.reset();
        since_ = now;
        from_since_ = now;
        began_ = now;
        last_came_ = now;
        take_maps();
        return;
    }
    // The fade spans the time between weathers when they come slower than the server told, so the
    // cloud keeps changing at one pace the whole way and is never caught short.
    auto fade = std::chrono::duration_cast<MonotonicClock::duration>(weather.fade);
    const auto apart = now - last_came_;
    if (fade > MonotonicClock::duration::zero() && apart <= max_apart) {
        fade = std::max(fade,
                        std::chrono::duration_cast<MonotonicClock::duration>(apart * fade_share));
    }
    last_came_ = now;
    waiting_ = Waiting{.cells = weather.cells, .came = now, .fade = fade};
    carry_at_pace(waiting_->cells, weather.pace);
    fill_heights(waiting_->cells);
    update(now);
}

void Forecast::update(MonotonicClock::time_point now) {
    if (!waiting_ || faded(now) < 1.0) {
        return;
    }
    // What's faded to is whole now, so it goes on as the weather faded from, drifting as it was.
    from_ = std::move(to_);
    from_since_ = since_;
    to_ = std::move(waiting_->cells);
    since_ = waiting_->came;
    fade_ = waiting_->fade;
    began_ = now;
    waiting_.reset();
    if (fade_ <= MonotonicClock::duration::zero()) {
        from_ = to_;
        from_since_ = since_;
    }
    take_maps();
}

void Forecast::take_maps() {
    ++received_;
    Maps before = maps_of(*grid_, from_);
    Maps after = fade_ > MonotonicClock::duration::zero() ? maps_of(*grid_, to_) : before;
    before_ = std::move(before.weather);
    sky_before_ = std::move(before.sky);
    drift_before_ = std::move(before.drift);
    after_ = std::move(after.weather);
    sky_after_ = std::move(after.sky);
    drift_after_ = std::move(after.drift);
}

double Forecast::faded(MonotonicClock::time_point now) const noexcept {
    if (fade_ <= MonotonicClock::duration::zero()) {
        return 1.0;
    }
    return std::clamp(std::chrono::duration<double>(now - began_) /
                          std::chrono::duration<double>(fade_),
                      0.0, 1.0);
}

protocol::CellWeather Forecast::at(DVec3 direction, MonotonicClock::time_point now) const {
    if (!grid_) {
        return {};
    }
    return shown(mixed(interpolated(*grid_, from_, direction), interpolated(*grid_, to_, direction),
                       static_cast<float>(faded(now))),
                 direction, now);
}

DVec3 Forecast::drifted(DVec3 direction, MonotonicClock::time_point now) const {
    if (!grid_) {
        return DVec3{0.0};
    }
    const auto blown = [&](const Cells& cells, double age) {
        const protocol::CellWeather here = interpolated(*grid_, cells, direction);
        return planet::tangent_frame(direction).to_world(
                   {static_cast<double>(here.drift.x), static_cast<double>(here.drift.y), 0.0}) *
               age;
    };
    return glm::mix(blown(from_, age_before(now)), blown(to_, age(now)), faded(now));
}

double Forecast::age(MonotonicClock::time_point now) const noexcept {
    return std::max(std::chrono::duration<double>(now - since_).count(), 0.0);
}

double Forecast::age_before(MonotonicClock::time_point now) const noexcept {
    return std::max(std::chrono::duration<double>(now - from_since_).count(), 0.0);
}

protocol::CellWeather Forecast::shown(const protocol::CellWeather& here, DVec3 direction,
                                      MonotonicClock::time_point now) const {
    if (!grid_) {
        return here;
    }
    // Each weather's cloud is read where its own drift brought it from.
    const auto read = [&](const Cells& cells, double age) {
        const protocol::CellWeather here = interpolated(*grid_, cells, direction);
        const DVec3 blown = planet::tangent_frame(direction).to_world(
            {static_cast<double>(here.drift.x), static_cast<double>(here.drift.y), 0.0});
        return interpolated(*grid_, cells, glm::normalize((direction * radius_) - (blown * age)));
    };
    const protocol::CellWeather there =
        mixed(read(from_, age_before(now)), read(to_, age(now)), static_cast<float>(faded(now)));
    // The cloud blows on with the wind, with the heights it has; the air's damp, and so its fog,
    // keep to the slower pace of the air.
    protocol::CellWeather found = here;
    found.cloud = there.cloud;
    found.cover = there.cover;
    found.falling = there.falling;
    found.snow = there.snow;
    found.base = there.base;
    found.top = there.top;
    return found;
}

waves::SeaState Forecast::sea_at(DVec3 direction, MonotonicClock::time_point now) const {
    waves::SeaState sea;
    if (!grid_) {
        return sea;
    }
    const sim::CellGrid::Shares around = grid_->shares(direction);
    const double by = faded(now);
    const auto add = [&](const Cells& cells, double counts) {
        for (std::size_t k = 0; k < around.cells.size(); ++k) {
            const double weight = counts * around.weights.at(k) / around.total;
            if (weight <= 0.0) {
                continue;
            }
            const std::uint32_t index = around.cells.at(k);
            const planet::TangentFrame& frame = grid_->frame(index);
            const auto along = [&](Vec2 vector) {
                return (frame.east * static_cast<double>(vector.x)) +
                       (frame.north * static_cast<double>(vector.y));
            };
            const protocol::CellWeather& cell = cells[index];
            sea.current += along(cell.current) * weight;
            sea.wind += along(cell.wind) * weight;
            // Each system's variance counts as much as its cell does.
            const auto system = [&](const protocol::CellWaves& waves, double peakedness,
                                    double spread) {
                if (waves.height > 0.0F && waves.peak > 0.0F) {
                    sea.systems.push_back(
                        {.heading = glm::normalize(along(waves.heading)),
                         .height = static_cast<double>(waves.height) * std::sqrt(weight),
                         .peak = static_cast<double>(waves.peak),
                         .peakedness = peakedness,
                         .spread = spread});
                }
            };
            system(cell.wind_sea, waves::wind_sea_peakedness, 2.0);
            for (const protocol::CellWaves& swell : cell.swells) {
                system(swell, waves::swell_peakedness, waves::swell_spread);
            }
        }
    };
    add(from_, 1.0 - by);
    add(to_, by);
    return sea;
}

} // namespace hog::client
