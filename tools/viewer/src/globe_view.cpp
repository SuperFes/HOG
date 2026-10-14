#include <hog/sim/calendar.hpp>
#include <hog/viewer/daylight.hpp>
#include <hog/viewer/globe_view.hpp>

#include <algorithm>

namespace hog::viewer {

render::Camera GlobeCamera::camera() const noexcept {
    return render::orbit_camera(orbit_, 1.0, vertical_fov);
}

void GlobeCamera::drag(DVec2 pixels, double height) noexcept {
    orbit_ = render::dragged(orbit_, pixels, vertical_fov / std::max(height, 1.0), 1.0);
}

void GlobeCamera::zoom(double steps) noexcept {
    orbit_ = render::zoomed(orbit_, steps, altitudes);
}

render::GlobeView globe_view(const GlobeCamera& camera, const Frame* frame, GlobeLook look) {
    render::GlobeView view{.camera = camera.camera(),
                           .radius = 1.0,
                           .sun = DVec3{1.0, 0.0, 0.0},
                           .night_light = 1.0,
                           .twilight = sim::twilight,
                           .air = air_depth};
    if (frame == nullptr) {
        return view;
    }
    if (const double radius = frame->welcome().planet_radius; radius > 0.0) {
        view.relief = look.relief / radius;
        view.cloud_lift = std::max(look.relief, 1.0) * cloud_height / radius;
    }
    if (!look.daylight) {
        return view;
    }
    if (const auto calendar = calendar_of(frame->welcome())) {
        view.sun = calendar->sun_direction(frame->time());
        view.night_light = night_light;
    }
    return view;
}

} // namespace hog::viewer
