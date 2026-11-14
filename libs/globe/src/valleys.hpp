#pragma once

#include <hog/core/math.hpp>
#include <hog/formation/smooth.hpp>
#include <hog/globe/interval.hpp>

#include <array>
#include <cstddef>
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
    DVec3 wall_gradient{0.0}; // how the walls' steepness changes, per metre, where it varies
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
    // either side of the line at most, and metres from one bend to the next like it. Its depth,
    // and the water's in it, change along the whole valley from those at its head to those at
    // its foot, so it meets the next valley's channel and water as they are there.
    double channel_width = 0.0;
    double channel_depth = 0.0;
    double channel_depth_to = 0.0;
    double water_depth = 0.0;
    double water_depth_to = 0.0;
    // The lowest its water stands, as where it runs into a lake that holds it up.
    double least_water = -1e300;
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
    // Where the river's water stands along the line here, though only the channel lies under it,
    // and the banks it rises to in flood.
    double water = 0.0;
    double bank = 0.0;
    DVec3 fall{0.0};
    double reaching = 1.0;
    // How much of a wall the ground standing is, from none across the floor to all up the walls.
    double steep = 0.0;
    double standing = 0.0;
    DVec3 standing_gradient{0.0}; // per metre, as `gradient`
    DVec3 reaching_gradient{0.0}; // how `reaching` changes, per metre
    // How deep the channel cuts under `standing`.
    double cut = 0.0;
    DVec3 cut_gradient{0.0};
};
[[nodiscard]] Wall wall_at(const Valley& valley, DVec3 direction, double radius) noexcept;

// Bounds of where wall_at() has the ground stand anywhere within `within` radians of `direction`,
// as `standing` and with its rise out of the way as `height`, its walls as steep as anywhere in
// `steepness` in place of the valley's own.
struct WallBounds {
    Interval standing;
    Interval height;
};
[[nodiscard]] WallBounds wall_bounds(const Valley& valley, DVec3 direction, double within,
                                     double radius, Interval steepness) noexcept;

// Past half its reach, a wall rises away smoothly, `out_of_the_way` by its reach: how far, `away`
// metres from its line.
constexpr double out_of_the_way = 1e5;
[[nodiscard]] double rise_past(double away, double reach) noexcept;

// The lowest of several grounds, each standing over a floor and so much a wall, rounded where it
// meets another as far as both are walls: a quadratic smooth minimum, its slope shared between the
// two as they near. The crease is rounded over `rounding` across, in the gradients' units, so it
// merges over as much height as they part over that: none where they run alike, and no more than
// `blend`, as walls meeting head on part. Rounded by the one crest the lowest meets most, never by
// every one at once, which would sink it far below them all; and never lower than halfway down to
// its floor, handed over between floors as the ground is: what creeps off a crest never digs a
// floor out. It turns as each way comes lowest over `handing_over`, never at once, so a crest
// rounded from it, as wide as their gradients part, never steps.
class Lowered {
public:
    Lowered(double rounding, double blend) noexcept : rounding_{rounding}, blend_{blend} {}

    void add(const formation::Smooth::Read& ground, double wall, double floor) noexcept;
    [[nodiscard]] bool empty() const noexcept { return count_ == 0; }
    // Precondition: not empty().
    [[nodiscard]] formation::Smooth::Read lowest() const noexcept;

private:
    struct Ground {
        formation::Smooth::Read read;
        double wall = 0.0;
        double floor = 0.0;
    };
    // The floor `low` stands over, handed over from its own to those coming as low over the blend,
    // so none breaks where another comes lowest.
    [[nodiscard]] double floor_under(const Ground& low) const noexcept;
    double rounding_ = 0.0;
    double blend_ = 0.0;
    std::array<Ground, 32> grounds_{};
    std::size_t count_ = 0;
    std::size_t lowest_ = 0;
};

// Of the stretches of a winding valley, the ground stands as the lowest has it, channel and all:
// blending them would lift the floor up the walls of those beside and fill the channel in. Where
// their walls meet, at the spurs between bends, they round over `rounding` metres across, over no
// more than `blend` in height, as other crests do: the lowest rounded by the one crest it meets
// most, never by every stretch at once, which would sink it far below them all, nor lower than
// halfway down to its floor: what creeps off a crest never digs the floor out. The water stands
// at the level of whichever lies lowest, fading out over `handing_over` as another comes lower.
constexpr double handing_over = 1.0;

class Lowest {
public:
    Lowest(double rounding, double blend) noexcept : blend_{blend}, lowered_{rounding, blend} {}

    void add(const Wall& one) noexcept;
    // `wall`, blended, lowered to it.
    void lower(Wall& wall) const noexcept;

private:
    // A stretch's ground and the levels its water and floods stand at.
    struct Stretch {
        double standing = 0.0;
        double water = 0.0;
        double bank = 0.0;
    };
    double blend_ = 0.0;
    Lowered lowered_;
    double lowest_ = 0.0;
    double cut_ = 0.0;
    DVec3 cut_gradient_{0.0};
    DVec3 fall_{0.0};
    double floor_ = 0.0;
    std::array<Stretch, 32> stretches_{};
    std::size_t count_ = 0;
};

} // namespace hog::globe
