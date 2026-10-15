#pragma once

#include <hog/core/math.hpp>
#include <hog/render/camera.hpp>
#include <hog/render/vulkan.hpp>
#include <hog/terrain/selection.hpp>

#include <concepts>
#include <functional>

namespace hog::client {

// A free-flying eye over a planet centred on the origin, kept level with the ground: its up is
// always away from the centre, so the horizon stays level wherever it goes, the poles included.
struct Flight {
    // NOLINTNEXTLINE(readability-redundant-member-init): lets designated initializers omit it
    WorldPosition eye = {};
    DVec3 heading{1.0, 0.0, 0.0}; // unit, along the ground, the way it faces
    double pitch = 0.0;           // radians above the horizon
};

// What the controls ask of a frame.
struct FlyInput {
    DVec3 move{};         // right, up and forwards, each from -1 to 1
    DVec2 look{};         // pixels the mouse moved, x right and y down
    double seconds = 0.0; // since the last frame
};

struct LookSettings {
    double per_pixel = 0.002; // radians
    double steepest = 1.5;    // radians up or down the view can pitch
};

struct FlySettings {
    LookSettings look{};
    // Flying speed in heights above the ground a second, so the ground below seems to pass
    // as quickly at any height.
    double speed = 0.5;
    double slowest = 3.0; // metres a second
    double lowest = 2.0;  // metres above the ground the eye keeps
};

struct WalkSettings {
    LookSettings look{};
    double walking = 1.5;    // metres a second
    double running = 6.0;    // metres a second
    double eye_height = 3.0; // metres above the ground
};

// Over a unit `direction`, `altitude` metres above sea level, facing along the ground towards
// `towards` where it can; otherwise north, or at a pole along +x.
[[nodiscard]] Flight start_flight(DVec3 direction, double planet_radius, double altitude,
                                  DVec3 towards);

// The flight a frame of input later, `ground` being the height of the ground under the eye in
// metres above sea level.
[[nodiscard]] Flight fly(const Flight& flight, const FlyInput& input, double planet_radius,
                         double ground, const FlySettings& settings = {});

// A frame of walking along the ground the way the eye faces, staying as high as it is.
[[nodiscard]] Flight step(const Flight& flight, const FlyInput& input, bool running,
                          const WalkSettings& settings = {});

// The eye stood on `ground`, metres above sea level.
[[nodiscard]] Flight stand(const Flight& flight, double planet_radius, double ground,
                           const WalkSettings& settings = {});

// A frame of walking, standing on the ground wherever it gets to; `ground_at` gives the ground's
// height in metres above sea level under a position.
template <std::invocable<WorldPosition> GroundAt>
[[nodiscard]] Flight walk(const Flight& flight, const FlyInput& input, bool running,
                          double planet_radius, const GroundAt& ground_at,
                          const WalkSettings& settings = {}) {
    const Flight stepped = step(flight, input, running, settings);
    return stand(stepped, planet_radius, std::invoke(ground_at, stepped.eye), settings);
}

[[nodiscard]] render::Camera flight_camera(const Flight& flight, double vertical_fov);

// Where someone flying along with the eye is: `ahead` metres from it along the ground.
[[nodiscard]] DVec3 companion_direction(const Flight& flight, double ahead);

// Where they stand there, `below` metres under the eye's height but never under the ground,
// `ground` metres above sea level.
[[nodiscard]] WorldPosition companion_feet(const Flight& flight, double ahead, double below,
                                           double planet_radius, double ground);

// What the camera sees through a viewport of `extent`, for picking the patches to draw.
[[nodiscard]] terrain::Viewpoint viewpoint(const render::Camera& camera, VkExtent2D extent);

} // namespace hog::client
