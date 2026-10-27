#include <hog/client/flight.hpp>
#include <hog/planet/tangent_frame.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <numbers>
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

// Carried along the ground to where the eye now is, so it turns only when asked.
Flight carried(Flight flight) {
    flight.heading =
        along_ground(glm::normalize(flight.eye.metres()), flight.heading).value_or(flight.heading);
    return flight;
}

// Facing is sent in 65536ths of a turn.
constexpr double per_radian = 65'536.0 / (2.0 * std::numbers::pi);

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

Flight carried_to(const Flight& flight, WorldPosition eye) {
    Flight moved = flight;
    moved.eye = eye;
    return carried(moved);
}

double bearing_of(const Flight& flight) {
    const planet::TangentFrame frame = planet::tangent_frame(flight.eye.metres());
    return std::atan2(glm::dot(flight.heading, frame.east), glm::dot(flight.heading, frame.north));
}

Flight turned_to(const Flight& flight, double bearing, double pitch) {
    const planet::TangentFrame frame = planet::tangent_frame(flight.eye.metres());
    Flight next = flight;
    next.heading = (frame.east * std::sin(bearing)) + (frame.north * std::cos(bearing));
    next.pitch = pitch;
    return next;
}

protocol::Facing facing_of(const Flight& flight) {
    const double turned = bearing_of(flight);
    const double pitch = std::clamp(flight.pitch * per_radian, -32'768.0, 32'767.0);
    // Wrapped round the turn, so a heading just west of north is near the top.
    return {.heading = static_cast<std::uint16_t>(std::lround(turned * per_radian) & 0xFFFF),
            .pitch = static_cast<std::int16_t>(std::lround(pitch))};
}

Flight faced(const Flight& flight, protocol::Facing facing) {
    return turned_to(flight, static_cast<double>(facing.heading) / per_radian,
                     static_cast<double>(facing.pitch) / per_radian);
}

render::Camera flight_camera(const Flight& flight, double vertical_fov) {
    const DVec3 forward = forward_of(flight);
    return {.eye = flight.eye,
            .forward = forward,
            .up = glm::cross(right_of(flight), forward),
            .vertical_fov = vertical_fov};
}

render::Camera third_person(const Flight& flight, double distance, double vertical_fov) {
    render::Camera camera = flight_camera(flight, vertical_fov);
    camera.eye -= camera.forward * distance;
    return camera;
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
