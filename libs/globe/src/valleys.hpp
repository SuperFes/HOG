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
    // Where along the whole valley the line runs, 0 at its head to 1 at its foot, when it's one
    // stretch of a winding valley: water turns only at the valley's own ends.
    double head = 0.0;
    double foot = 1.0;
};

// How high the ground beside a valley may stand at `direction`, on a planet `radius` metres
// round: the valley's floor where nearest, rising up its walls with the distance from it; and
// which way it rises, per metre along the sphere; and the floor's height there. And the way water
// runs down it, which is up the wall itself, never up its rise out of the way; and how much of
// the wall's own is here, 1 within half its reach falling to nothing at it, and its height as it
// would stand there without rising away.
struct Wall {
    double height = 0.0;
    DVec3 gradient{0.0};
    double floor = 0.0;
    DVec3 fall{0.0};
    double reaching = 1.0;
    double standing = 0.0;
};
[[nodiscard]] Wall wall_at(const Valley& valley, DVec3 direction, double radius) noexcept;

} // namespace hog::globe
