#include <hog/client/daylight.hpp>
#include <hog/client/flight.hpp>
#include <hog/client/views.hpp>
#include <hog/core/time.hpp>
#include <hog/planet/projection.hpp>
#include <hog/planet/tangent_frame.hpp>
#include <hog/terrain/bend.hpp>

#include <array>
#include <chrono>
#include <cmath>
#include <numbers>

namespace hog::client {

namespace {

constexpr double degree = std::numbers::pi / 180.0;

// The islands lie about 42° south and 34° west, about 2 km to a degree of latitude.
constexpr std::array views{
    FixedView{.name = "islands-noon",
              .place = {.latitude = -44.0, .longitude = -33.86, .altitude = 600.0},
              .heading = 0.0,
              .pitch = -10.0,
              .time_of_day = 0.5},
    FixedView{.name = "shore-morning",
              .place = {.latitude = -40.3, .longitude = -33.86, .altitude = 30.0},
              .heading = 180.0,
              .pitch = -3.0,
              .time_of_day = 8.0 / 24.0},
    FixedView{.name = "sun-morning",
              .place = {.latitude = -42.13, .longitude = -35.9, .altitude = 50.0},
              .heading = 80.0,
              .pitch = 5.0,
              .time_of_day = 7.0 / 24.0},
    FixedView{.name = "sea-sunset",
              .place = {.latitude = -42.13, .longitude = -35.9, .altitude = 50.0},
              .heading = 215.0,
              .pitch = 0.0,
              .time_of_day = 17.95 / 24.0},
    FixedView{.name = "high-horizon",
              .place = {.latitude = -46.0, .longitude = -33.86, .altitude = 20'000.0},
              .heading = 0.0,
              .pitch = -40.0,
              .time_of_day = 10.0 / 24.0},
    FixedView{.name = "islands-night",
              .place = {.latitude = -44.0, .longitude = -33.86, .altitude = 600.0},
              .heading = 0.0,
              .pitch = -10.0,
              .time_of_day = 23.0 / 24.0},
};

// The March equinox, with the sun over the equator.
GameTime fixed_date() {
    using namespace std::chrono;
    return to_game_time(WallClock::time_point{sys_days{year{2026} / 3 / 20}});
}

} // namespace

std::span<const FixedView> fixed_views() noexcept {
    return views;
}

Shot shot_of(const FixedView& view, const planet::World& world, const sim::Calendar& calendar) {
    const double radius = world.geology().planet_radius();
    const DVec3 up =
        planet::direction_at(view.place.latitude * degree, view.place.longitude * degree);
    const planet::TangentFrame frame = planet::tangent_frame(up);
    Flight flight = start_flight(
        up, radius, view.place.altitude,
        frame.to_world({std::sin(view.heading * degree), std::cos(view.heading * degree), 0.0}));
    flight.pitch = view.pitch * degree;
    return {.camera = flight_camera(flight, vertical_fov),
            .bend = terrain::bend_from(flight.eye, radius, ground_under(world, flight.eye)),
            .time = start_time(calendar, fixed_date(), up, view.time_of_day),
            .seconds = 0.0,
            .companion = std::nullopt};
}

} // namespace hog::client
