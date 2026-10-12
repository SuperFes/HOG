#include <hog/viewer/map_view.hpp>

#include <algorithm>

namespace hog::viewer {

DVec2 map_point(const MapRect& shown, DVec2 at) noexcept {
    return shown.low + ((shown.high - shown.low) * at);
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
