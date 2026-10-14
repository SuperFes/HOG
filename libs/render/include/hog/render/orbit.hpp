#pragma once

#include <hog/core/math.hpp>
#include <hog/render/camera.hpp>

namespace hog::render {

// A camera circling a planet centred on the origin, looking at its centre with north up.
struct Orbit {
    double latitude = 0.0;  // radians
    double longitude = 0.0; // radians
    double altitude = 1.0;  // metres above the planet's surface

    friend bool operator==(const Orbit&, const Orbit&) = default;
};

// How low and how high an orbit may go, in metres above the surface.
struct AltitudeRange {
    double lowest = 1.0;
    double highest = 1.0;
};

[[nodiscard]] Camera orbit_camera(const Orbit& orbit, double planet_radius,
                                  double vertical_fov) noexcept;

// The orbit after dragging the view by `pixels` (x right, y down), each spanning `pixel_angle`
// radians of view, so the ground under the cursor roughly follows it.
[[nodiscard]] Orbit dragged(Orbit orbit, DVec2 pixels, double pixel_angle,
                            double planet_radius) noexcept;

// The orbit after `steps` of zooming in, or out for negative steps, by a share of the altitude.
[[nodiscard]] Orbit zoomed(Orbit orbit, double steps, AltitudeRange range) noexcept;

} // namespace hog::render
