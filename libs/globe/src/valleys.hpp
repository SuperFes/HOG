#pragma once

#include <hog/core/math.hpp>

#include <cstdint>

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
    // Metres along the whole valley, head to foot.
    double length = 0.0;
    // Metres either side of the line its floor runs flat, where its river wanders.
    double belt = 0.0;
    // The river's channel winding along the floor: metres across and deep, metres it bends
    // either side of the line at most, and metres from one bend to the next like it.
    double channel_width = 0.0;
    double channel_depth = 0.0;
    double meander = 0.0;
    double meander_length = 1.0;
    std::uint64_t seed = 0;
    // Which way the valley runs at each end of the line, where it bends on to the next stretch;
    // none for a valley straight between its ends.
    DVec3 runs_from{0.0};
    DVec3 runs_to{0.0};
};

// How high the ground beside a valley may stand at `direction`, on a planet `radius` metres
// round: the valley's floor where nearest, flat across its belt but for the channel cut in it,
// rising up its walls past the belt with the distance from it; and which way it rises, per
// metre along the sphere; and the floor's height there. And the way water runs down it: up the
// wall itself, never up its rise out of the way, and along the floor; and how much of the wall's
// own is here, 1 within half its reach falling to nothing at it, and its height as it would stand
// there without rising away.
struct Wall {
    double height = 0.0;
    DVec3 gradient{0.0};
    double floor = 0.0;
    DVec3 fall{0.0};
    double reaching = 1.0;
    double standing = 0.0;
    DVec3 standing_gradient{0.0}; // per metre, as `gradient`
    DVec3 reaching_gradient{0.0}; // how `reaching` changes, per metre
};
[[nodiscard]] Wall wall_at(const Valley& valley, DVec3 direction, double radius) noexcept;

// Past half its reach, a wall rises away smoothly, `out_of_the_way` by its reach: how far, `away`
// metres from its line.
constexpr double out_of_the_way = 1e5;
[[nodiscard]] double rise_past(double away, double reach) noexcept;

} // namespace hog::globe
