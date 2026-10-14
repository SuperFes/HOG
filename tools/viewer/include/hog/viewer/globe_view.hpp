#pragma once

#include <hog/core/math.hpp>
#include <hog/render/globe.hpp>
#include <hog/render/orbit.hpp>
#include <hog/viewer/frame.hpp>

namespace hog::viewer {

// The globe's camera, orbiting a planet of unit radius.
class GlobeCamera {
public:
    static constexpr double vertical_fov = 0.8; // radians
    static constexpr render::AltitudeRange altitudes{.lowest = 0.02, .highest = 8.0};
    static constexpr double start_altitude = 2.0;

    [[nodiscard]] const render::Orbit& orbit() const noexcept { return orbit_; }
    [[nodiscard]] render::Camera camera() const noexcept;

    // Drags by `pixels` across a view `height` pixels tall, so the ground follows the cursor.
    void drag(DVec2 pixels, double height) noexcept;
    // Zooms in by `steps`, or out for negative steps.
    void zoom(double steps) noexcept;
    void reset() noexcept { *this = {}; }

private:
    render::Orbit orbit_{.latitude = 0.0, .longitude = 0.0, .altitude = start_altitude};
};

// How the globe is shown.
struct GlobeLook {
    bool daylight = true;
    // Times over the land's true height it stands, once the planet's size is known.
    double relief = 1.0;
};

// The globe as seen from `camera`, lit by the sun as of `frame` when daylight is shown and the
// server sent a calendar, and evenly otherwise.
[[nodiscard]] render::GlobeView globe_view(const GlobeCamera& camera, const Frame* frame,
                                           GlobeLook look);

} // namespace hog::viewer
