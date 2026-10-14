#include <hog/planet/projection.hpp>
#include <hog/planet/tangent_frame.hpp>
#include <hog/render/orbit.hpp>

#include <algorithm>
#include <cmath>
#include <numbers>

namespace hog::render {

namespace {

// Short of the poles, where north stops pointing anywhere useful.
constexpr double max_latitude = std::numbers::pi * 89.9 / 180.0;
// Altitude kept per step zoomed in.
constexpr double zoom_share = 0.8;
// Of the altitude, so the nearest ground is always well beyond the near plane.
constexpr double near_share = 0.1;
// Holds dragging near the poles to a sane speed.
constexpr double least_parallel = 0.1;

} // namespace

Camera orbit_camera(const Orbit& orbit, double planet_radius, double vertical_fov) noexcept {
    const DVec3 direction = planet::direction_at(orbit.latitude, orbit.longitude);
    return {.eye = WorldPosition{direction * (planet_radius + orbit.altitude)},
            .forward = -direction,
            .up = planet::tangent_frame(direction).north,
            .vertical_fov = vertical_fov,
            .near_plane = std::max(orbit.altitude * near_share, 0.01)};
}

Orbit dragged(Orbit orbit, DVec2 pixels, double pixel_angle, double planet_radius) noexcept {
    // Seen from straight above, a pixel spans about altitude * pixel_angle metres of ground.
    const double arc = orbit.altitude * pixel_angle / planet_radius;
    orbit.latitude = std::clamp(orbit.latitude + (pixels.y * arc), -max_latitude, max_latitude);
    const double parallel = std::max(std::cos(orbit.latitude), least_parallel);
    orbit.longitude =
        std::remainder(orbit.longitude - (pixels.x * arc / parallel), 2.0 * std::numbers::pi);
    return orbit;
}

Orbit zoomed(Orbit orbit, double steps, AltitudeRange range) noexcept {
    orbit.altitude =
        std::clamp(orbit.altitude * std::pow(zoom_share, steps), range.lowest, range.highest);
    return orbit;
}

} // namespace hog::render
