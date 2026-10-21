#include <hog/planet/projection.hpp>
#include <hog/terrain/shore.hpp>

#include <algorithm>

namespace hog::terrain {

ShoreFrame shore_frame(DVec3 eye, double radius) {
    const int level = planet::level_for(shore_anchoring, radius);
    const DVec3 anchor = planet::cell_center(planet::cell_at(eye, level));
    // Any way across will do at the poles, where east has none.
    DVec3 across = glm::cross(DVec3{0.0, 0.0, 1.0}, anchor);
    if (glm::length(across) < 1e-6) {
        across = glm::cross(DVec3{1.0, 0.0, 0.0}, anchor);
    }
    const DVec3 east = glm::normalize(across);
    return {.anchor = anchor, .east = east, .north = glm::cross(anchor, east)};
}

DVec3 shore_direction(const ShoreFrame& frame, double radius, DVec2 along) {
    return glm::normalize((frame.anchor * radius) + (frame.east * along.x) +
                          (frame.north * along.y));
}

DVec2 shore_along(const ShoreFrame& frame, double radius, DVec3 direction) {
    const double scale = radius / glm::dot(direction, frame.anchor);
    return DVec2{glm::dot(direction, frame.east), glm::dot(direction, frame.north)} * scale;
}

std::vector<float> shore_depths(const planet::Elevation& terrain, const ShoreFrame& frame,
                                double radius, double spacing) {
    std::vector<float> depths;
    depths.reserve(static_cast<std::size_t>(shore_size) * shore_size);
    const double first = (0.5 - (shore_size / 2.0)) * spacing;
    for (int row = 0; row < shore_size; ++row) {
        for (int column = 0; column < shore_size; ++column) {
            const DVec3 at = shore_direction(frame, radius,
                                             {first + (column * spacing), first + (row * spacing)});
            const double elevation =
                planet::fixed_to_double(terrain.elevation(planet::cell_at(at)));
            depths.push_back(std::max(static_cast<float>(-elevation), shore_highest));
        }
    }
    return depths;
}

void ShoreStream::follow(DVec3 eye) {
    wanted_ = shore_frame(eye, radius_);
    if (!working_ && wanted_ != started_) {
        start(*wanted_);
    }
}

void ShoreStream::start(const ShoreFrame& frame) {
    started_ = frame;
    working_ = std::make_unique<jobs::Async<ShoreDepths>>(
        *jobs_, [terrain = terrain_, frame, radius = radius_] {
            ShoreDepths found{.frame = frame, .depths = {}};
            for (const double spacing : shore_spacing) {
                const std::vector<float> layer = shore_depths(*terrain, frame, radius, spacing);
                found.depths.insert(found.depths.end(), layer.begin(), layer.end());
            }
            return found;
        });
}

std::optional<ShoreDepths> ShoreStream::collect() {
    if (!working_ || !working_->done()) {
        return std::nullopt;
    }
    ShoreDepths found = working_->take();
    working_.reset();
    if (wanted_ && wanted_ != started_) {
        start(*wanted_);
    }
    return found;
}

std::optional<ShoreDepths> ShoreStream::finish() {
    std::optional<ShoreDepths> last;
    while (working_) {
        last = working_->take();
        working_.reset();
        if (wanted_ && wanted_ != started_) {
            start(*wanted_);
        }
    }
    return last;
}

} // namespace hog::terrain
