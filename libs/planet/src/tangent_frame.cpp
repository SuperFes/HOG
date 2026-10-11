#include <hog/core/assert.hpp>
#include <hog/planet/projection.hpp>
#include <hog/planet/tangent_frame.hpp>

namespace hog::planet {

TangentFrame tangent_frame(DVec3 direction) {
    const double length = glm::length(direction);
    HOG_ASSERT(length > 0.0, "direction must be non-zero");

    const DVec3 up = direction / length;
    DVec3 east = glm::cross(north_pole, up);
    // Within a few micrometres of a pole on an Earth-sized planet, east is meaningless.
    constexpr double pole_epsilon = 1e-12;
    east = glm::length(east) > pole_epsilon ? glm::normalize(east) : DVec3{1.0, 0.0, 0.0};
    return {.east = east, .north = glm::cross(up, east), .up = up};
}

TangentFrame tangent_frame(CellId cell) {
    return tangent_frame(cell_center(cell));
}

TangentFrame tangent_frame(WorldPosition position) {
    return tangent_frame(position.metres());
}

} // namespace hog::planet
