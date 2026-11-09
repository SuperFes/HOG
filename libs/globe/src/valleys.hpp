#pragma once

#include <hog/core/math.hpp>

namespace hog::globe {

// A valley along the line between two directions from the planet's centre: its floor's height
// at each end, how steeply its walls rise from it, as metres up per metre across, and how many
// metres from it they reach, past half of which they rise out of the way of all else.
struct Valley {
    DVec3 from{0.0};
    DVec3 to{0.0};
    double floor_from = 0.0;
    double floor_to = 0.0;
    double wall = 0.0;
    double reach = 1e30;
};

// How high the ground beside a valley may stand at `direction`, on a planet `radius` metres
// round: the valley's floor where nearest, rising up its walls with the distance from it; and
// which way it rises, per metre along the sphere.
struct Wall {
    double height = 0.0;
    DVec3 gradient{0.0};
};
[[nodiscard]] Wall wall_at(const Valley& valley, DVec3 direction, double radius) noexcept;

} // namespace hog::globe
