#pragma once

#include <hog/core/math.hpp>
#include <hog/render/camera.hpp>
#include <hog/render/vulkan.hpp>
#include <hog/terrain/selection.hpp>

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

struct FlySettings {
    double look_per_pixel = 0.002; // radians
    // Flying speed in heights above the ground a second, so the ground below seems to pass
    // as quickly at any height.
    double speed = 0.5;
    double slowest = 3.0;  // metres a second
    double lowest = 2.0;   // metres above the ground the eye keeps
    double steepest = 1.5; // radians up or down the view can pitch
};

// Over a unit `direction`, `altitude` metres above sea level, facing along the ground towards
// `towards` where it can; otherwise north, or at a pole along +x.
[[nodiscard]] Flight start_flight(DVec3 direction, double planet_radius, double altitude,
                                  DVec3 towards);

// The flight a frame of input later, `ground` being the height of the ground under the eye in
// metres above sea level.
[[nodiscard]] Flight fly(const Flight& flight, const FlyInput& input, double planet_radius,
                         double ground, const FlySettings& settings = {});

[[nodiscard]] render::Camera flight_camera(const Flight& flight, double vertical_fov);

// What the camera sees through a viewport of `extent`, for picking the patches to draw.
[[nodiscard]] terrain::Viewpoint viewpoint(const render::Camera& camera, VkExtent2D extent);

} // namespace hog::client
