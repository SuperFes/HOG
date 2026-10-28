#pragma once

#include <hog/core/math.hpp>
#include <hog/core/time.hpp>
#include <hog/protocol/messages.hpp>
#include <hog/sim/cell_grid.hpp>
#include <hog/waves/waves.hpp>

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace hog::client {

// Texels along a side of each face of the weather's cube maps: twice the finest weather sent.
inline constexpr std::uint32_t weather_map_size = 64;

// A texel of the weather's cube maps, each part from 0 to 255 for 0 to 1: the share of the sky
// clouded, the cloud's water as the square root of a sixteenth of its millimetres, what falls as
// the square root of a 256th of its millimetres an hour, and the share of that falling as snow.
using WeatherTexel = std::array<std::uint8_t, 4>;

// A texel of the sky's cube maps, as protocol::CellWeather sends them: the humidity near the ground
// from 0 to 255 for 0 to 1, the fog's top in steps of `fog_texel_step` metres above sea level, and
// the cloud's base and top in steps of `height_texel_step`.
using SkyTexel = std::array<std::uint8_t, 4>;
inline constexpr double fog_texel_step = 32.0;
inline constexpr double height_texel_step = 64.0;

// The unit direction through the middle of texel (x, y), counted from the top left, of `face` of a
// cube map `size` texels a side, as Vulkan samples it; faces go +x, -x, +y, -y, +z, -z.
[[nodiscard]] DVec3 cube_direction(std::uint32_t face, std::uint32_t x, std::uint32_t y,
                                   std::uint32_t size) noexcept;

// The weather as a line for people to read.
[[nodiscard]] std::string describe(const protocol::CellWeather& weather);

// The weather the server last sent, faded to from what was shown when it came, and its cloud
// drifted on with the weather since, until the next comes.
class Forecast {
public:
    // On a planet whose sea level lies `radius` metres from its centre.
    explicit Forecast(double radius) noexcept : radius_(radius) {}

    // Fades from what's shown at `now`, where it's drifted to, to `weather` over its fade. The
    // first weather, or weather at another level from what's shown, is shown at once.
    void receive(const protocol::Weather& weather, MonotonicClock::time_point now);

    [[nodiscard]] bool known() const noexcept { return grid_.has_value(); }
    // How far what's shown at `now` has faded from before() to after(), from 0 to 1.
    [[nodiscard]] double faded(MonotonicClock::time_point now) const noexcept;
    // The weather at a unit direction as shown at `now`; a clear sky while none is known.
    [[nodiscard]] protocol::CellWeather at(DVec3 direction, MonotonicClock::time_point now) const;
    // Metres the cloud shown at a unit direction at `now` has drifted along the ground since the
    // weather came, in the planet's frame.
    [[nodiscard]] DVec3 drifted(DVec3 direction, MonotonicClock::time_point now) const;
    // The sea at a unit direction as shown at `now`, in the planet's frame: the wave systems of
    // the cells around it, each as much as that cell counts there, so the waves change smoothly
    // from cell to cell. A calm sea while none is known.
    [[nodiscard]] waves::SeaState sea_at(DVec3 direction, MonotonicClock::time_point now) const;

    // Cube maps of the weather faded from and to, face by face, each row by row from the top;
    // empty while none is known.
    [[nodiscard]] const std::vector<WeatherTexel>& before() const noexcept { return before_; }
    [[nodiscard]] const std::vector<WeatherTexel>& after() const noexcept { return after_; }
    // As before() and after(), of the sky.
    [[nodiscard]] const std::vector<SkyTexel>& sky_before() const noexcept { return sky_before_; }
    [[nodiscard]] const std::vector<SkyTexel>& sky_after() const noexcept { return sky_after_; }
    // How many times weather has come, so the maps need only be taken when it changes.
    [[nodiscard]] std::uint64_t received() const noexcept { return received_; }

private:
    // Of the cells faded from and to, as shown at `now` at a unit direction, the cloud as it lay
    // where it's drifted from, under the air here.
    [[nodiscard]] protocol::CellWeather shown(const protocol::CellWeather& here, DVec3 direction,
                                              MonotonicClock::time_point now) const;

    double radius_;
    std::optional<sim::CellGrid> grid_;
    std::vector<protocol::CellWeather> from_;
    std::vector<protocol::CellWeather> to_;
    std::vector<WeatherTexel> before_;
    std::vector<WeatherTexel> after_;
    std::vector<SkyTexel> sky_before_;
    std::vector<SkyTexel> sky_after_;
    MonotonicClock::time_point since_{};
    MonotonicClock::duration fade_{};
    std::uint64_t received_ = 0;
};

} // namespace hog::client
