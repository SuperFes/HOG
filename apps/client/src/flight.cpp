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

// Turned and pitched as the mouse moved.
Flight looked(const Flight& flight, DVec2 look, const LookSettings& settings) {
    const DVec3 up = glm::normalize(flight.eye.metres());
    // Turning right is turning clockwise about up, seen from above.
    const double yaw = -look.x * settings.per_pixel;
    return {.eye = flight.eye,
            .heading =
                (flight.heading * std::cos(yaw)) + (glm::cross(up, flight.heading) * std::sin(yaw)),
            .pitch = std::clamp(flight.pitch - (look.y * settings.per_pixel), -settings.steepest,
                                settings.steepest)};
}

// Carried along the ground to where the eye now is, so it turns only when asked.
Flight carried(Flight flight) {
    flight.heading =
        along_ground(glm::normalize(flight.eye.metres()), flight.heading).value_or(flight.heading);
    return flight;
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
    Flight next = looked(flight, input.look, settings.look);

    const double floor = planet_radius + ground + settings.lowest;
    const double height = std::max(glm::length(flight.eye.metres()) - floor, 0.0);
    const double speed = std::max(settings.slowest, height * settings.speed);
    next.eye += ((right_of(next) * input.move.x) + (up * input.move.y) +
                 (forward_of(next) * input.move.z)) *
                (speed * input.seconds);

    if (glm::length(next.eye.metres()) < floor) {
        next.eye = WorldPosition{glm::normalize(next.eye.metres()) * floor};
    }
    return carried(next);
}

Flight step(const Flight& flight, const FlyInput& input, bool running,
            const WalkSettings& settings) {
    const Flight next = looked(flight, input.look, settings.look);
    const DVec3 way = (right_of(next) * input.move.x) + (next.heading * input.move.z);
    const double ways = glm::length(way);
    if (ways == 0.0) {
        return next;
    }
    const double from_centre = glm::length(next.eye.metres());
    const DVec3 up = next.eye.metres() / from_centre;
    // Diagonally no faster than straight.
    const double turned = std::min(ways, 1.0) * (running ? settings.running : settings.walking) *
                          input.seconds / from_centre;
    Flight walked = next;
    walked.eye =
        WorldPosition{((up * std::cos(turned)) + (way * (std::sin(turned) / ways))) * from_centre};
    return carried(walked);
}

Flight stand(const Flight& flight, double planet_radius, double ground,
             const WalkSettings& settings) {
    Flight stood = flight;
    stood.eye = WorldPosition{glm::normalize(flight.eye.metres()) *
                              (planet_radius + ground + settings.eye_height)};
    return stood;
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
