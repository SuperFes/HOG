#include <hog/planet/projection.hpp>
#include <hog/viewer/map_view.hpp>

#include <algorithm>
#include <numbers>

namespace hog::viewer {

DVec2 map_point(const MapRect& shown, DVec2 at) noexcept {
    return shown.low + ((shown.high - shown.low) * at);
}

DVec2 shown_point(const MapRect& shown, DVec2 point) noexcept {
    return (point - shown.low) / (shown.high - shown.low);
}

DVec2 pixel_point(const MapRect& shown, int width, int height, int x, int y) noexcept {
    return map_point(shown, DVec2{(x + 0.5) / width, (y + 0.5) / height});
}

DVec3 map_direction(DVec2 point) noexcept {
    return planet::direction_at((0.5 - point.y) * std::numbers::pi,
                                (point.x - 0.5) * 2.0 * std::numbers::pi);
}

MapRect MapView::shown() const noexcept {
    const double half = 0.5 / zoom_;
    return MapRect{.low = centre_ - half, .high = centre_ + half};
}

void MapView::zoom_by(double factor, DVec2 at) noexcept {
    const DVec2 under = map_point(shown(), at);
    zoom_ = std::clamp(zoom_ * factor, 1.0, most_zoom);
    // Whatever is under `at` stays there: the centre sits that far back from it at the new size.
    centre_ = under - ((at - 0.5) / zoom_);
    hold();
}

void MapView::pan(DVec2 by) noexcept {
    centre_ -= by / zoom_;
    hold();
}

void MapView::hold() noexcept {
    const double half = 0.5 / zoom_;
    centre_ = glm::clamp(centre_, DVec2{half}, DVec2{1.0 - half});
}

} // namespace hog::viewer
