#include <hog/client/flight.hpp>
#include <hog/planet/tangent_frame.hpp>

#include <algorithm>
#include <cmath>
#include <optional>

namespace hog::client {

namespace {

// The way along the ground from `up` that `towards` lies, if it lies any way along it.
std::optional<DVec3> along_ground(DVec3 up, DVec3 towards) {
    const DVec3 flat = towards - (up * glm::dot(towards, up));
    constexpr double least = 1e-9;
    if (glm::length(flat) < least * std::max(glm::length(towards), 1.0)) {
        return std::nullopt;
    }
    return glm::normalize(flat);
}

DVec3 right_of(const Flight& flight) {
    return glm::cross(flight.heading, glm::normalize(flight.eye.metres()));
}

DVec3 forward_of(const Flight& flight) {
    return (flight.heading * std::cos(flight.pitch)) +
           (glm::normalize(flight.eye.metres()) * std::sin(flight.pitch));
}

} // namespace

Flight start_flight(DVec3 direction, double planet_radius, double altitude, DVec3 towards) {
    const DVec3 up = glm::normalize(direction);
    const DVec3 heading = along_ground(up, towards)
                              .or_else([&] { return along_ground(up, planet::north_pole); })
                              .value_or(DVec3{1.0, 0.0, 0.0});
    return {.eye = WorldPosition{up * (planet_radius + altitude)}, .heading = heading};
}

Flight fly(const Flight& flight, const FlyInput& input, double planet_radius, double ground,
           const FlySettings& settings) {
    const DVec3 up = glm::normalize(flight.eye.metres());
    // Turning right is turning clockwise about up, seen from above.
    const double yaw = -input.look.x * settings.look_per_pixel;
    Flight next{.eye = flight.eye,
                .heading = (flight.heading * std::cos(yaw)) +
                           (glm::cross(up, flight.heading) * std::sin(yaw)),
                .pitch = std::clamp(flight.pitch - (input.look.y * settings.look_per_pixel),
                                    -settings.steepest, settings.steepest)};

    const double floor = planet_radius + ground + settings.lowest;
    const double height = std::max(glm::length(flight.eye.metres()) - floor, 0.0);
    const double speed = std::max(settings.slowest, height * settings.speed);
    next.eye += ((right_of(next) * input.move.x) + (up * input.move.y) +
                 (forward_of(next) * input.move.z)) *
                (speed * input.seconds);

    const DVec3 moved_up = glm::normalize(next.eye.metres());
    if (glm::length(next.eye.metres()) < floor) {
        next.eye = WorldPosition{moved_up * floor};
    }
    // Carried along the ground to where the eye now is, so it turns only when asked.
    next.heading = along_ground(moved_up, next.heading).value_or(next.heading);
    return next;
}

render::Camera flight_camera(const Flight& flight, double vertical_fov) {
    const DVec3 forward = forward_of(flight);
    return {.eye = flight.eye,
            .forward = forward,
            .up = glm::cross(right_of(flight), forward),
            .vertical_fov = vertical_fov};
}

DVec3 companion_direction(const Flight& flight, double ahead) {
    return glm::normalize(flight.eye.metres() + (flight.heading * ahead));
}

WorldPosition companion_feet(const Flight& flight, double ahead, double below, double planet_radius,
                             double ground) {
    const double height =
        std::max(glm::length(flight.eye.metres()) - planet_radius - below, ground);
    return WorldPosition{companion_direction(flight, ahead) * (planet_radius + height)};
}

terrain::Viewpoint viewpoint(const render::Camera& camera, VkExtent2D extent) {
    const auto height = static_cast<double>(std::max(extent.height, 1U));
    return {.eye = camera.eye,
            .forward = camera.forward,
            .up = camera.up,
            .vertical_fov = camera.vertical_fov,
            .aspect = static_cast<double>(extent.width) / height,
            .viewport_height = height};
}

} // namespace hog::client
