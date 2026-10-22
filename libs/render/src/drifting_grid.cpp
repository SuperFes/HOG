#include <hog/planet/tangent_frame.hpp>
#include <hog/render/drifting_grid.hpp>

#include <cmath>
#include <cstdlib>

namespace hog::render {

GridStep DriftingGrid::follow(DVec3 up, double radius, DVec3 drift, double seconds) {
    bool kept = plane_.has_value() && seconds >= 0.0 && seconds <= grid_longest_step;
    if (plane_) {
        const DVec2 along{glm::dot(up, plane_->east), glm::dot(up, plane_->north)};
        if (glm::dot(up, plane_->touching) <= 0.0 ||
            glm::length(along) * radius > grid_plane_reach) {
            plane_.reset();
        }
    }
    if (!plane_) {
        const planet::TangentFrame frame = planet::tangent_frame(up);
        plane_ = Plane{.touching = up, .east = frame.east, .north = frame.north};
        drifted_ = DVec2{0.0};
        kept = false;
    } else if (kept) {
        drifted_ += DVec2{glm::dot(drift, plane_->east), glm::dot(drift, plane_->north)} * seconds;
    }
    const DVec2 eye =
        DVec2{glm::dot(up, plane_->east), glm::dot(up, plane_->north)} * radius - drifted_;
    const glm::ivec2 centre{static_cast<int>(std::lround(eye.x / spacing_)),
                            static_cast<int>(std::lround(eye.y / spacing_))};
    const glm::ivec2 shift = centre - centre_;
    centre_ = centre;
    const int size = static_cast<int>(size_);
    if (std::abs(shift.x) >= size || std::abs(shift.y) >= size) {
        kept = false;
    }
    return {.shift = shift, .seconds = seconds, .kept = kept};
}

GridPlacing DriftingGrid::placing(DVec3 up, double radius, double height) const {
    if (!plane_) {
        return {.spacing = spacing_};
    }
    const glm::ivec2 half{static_cast<int>(size_ / 2)};
    return {.anchor = (plane_->touching * radius) - (up * (radius + height)),
            .east = plane_->east,
            .north = plane_->north,
            .corner = drifted_ + (DVec2{centre_ - half} * spacing_),
            .drifted = drifted_,
            .spacing = spacing_};
}

DVec2 grid_texel(const GridPlacing& placing, DVec3 rest) noexcept {
    const DVec3 from_anchor = rest - placing.anchor;
    return (DVec2{glm::dot(from_anchor, placing.east), glm::dot(from_anchor, placing.north)} -
            placing.corner) /
           placing.spacing;
}

} // namespace hog::render
