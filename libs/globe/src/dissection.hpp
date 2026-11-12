#pragma once

#include <hog/core/math.hpp>
#include <hog/globe/noise.hpp>

#include <cstdint>

namespace hog::globe {

// Ground as formed, before the gullies too fine for its points to hold are cut down it.
struct Undissected {
    Noise top; // metres, its gradient per metre along the sphere
    // Which way water runs off and how steeply, per metre: the top's own gradient, blended where
    // it turns sharply, over crests and where walls meet, so gullies fade there and never break.
    DVec3 fall{0.0};
    // Which way it falls on the whole, smoothed over the widest gullies' width, per metre.
    DVec3 broad_fall{0.0};
    // How far it stands over the floor of the valley it drains to, below which nothing cuts.
    double above_floor = 1e30;
    DVec3 above_floor_gradient{0.0}; // per metre
    double steepest = 0.0;           // the steepest its walls stand, metres up per metre across
    double ice = 0.0;                // metres of ice the top includes
};

struct Dissected {
    Noise top;
    double ice = 0.0;
    double cut = 0.0; // metres of the bed the gullies took off, negative where crests stand over it
    DVec3 cut_gradient{0.0}; // per metre
    // How far the bed about stands either side of its uncut height, by its gullies' floors and the
    // crests between, its gradient per metre: what's laid over it settles into that.
    Noise spread;
};

// How high over its bed's mean what lies `thick` deep over it on the whole stands, settled into the
// bed's hollows first, the bed's heights spread evenly `spread` either side of their mean: level
// over all of it once as deep as they spread. With how that changes as each does.
struct Settled {
    double level = 0.0;
    double per_thick = 1.0;
    double per_spread = 0.0;
};
[[nodiscard]] Settled settled(double thick, double spread) noexcept;

// Gullies `longest` metres apart and finer ones down their walls, each `octaves` half the last.
struct Gullies {
    double longest = 0.0;
    int octaves = 0;
    std::uint64_t seed = 0;
};

// The ground at `direction` on a planet `radius` metres round, with gullies cut down it along the
// way water runs: as deep as the ground falls over their own width, so flats are barely creased,
// and no deeper than their walls can stand across them, so slopes cut to crests between them. Each
// finer one runs the way the ground falls, cut into the walls of those before and branching off
// them, and none cuts below what it drains to. Gullies narrower than twice `lod` are left out, the
// last faded in. Ice fills them, as deep as it lay, so where it's thin the crests between them
// stand out of it.
[[nodiscard]] Dissected dissect(DVec3 direction, double radius, const Undissected& ground,
                                const Gullies& gullies, double lod) noexcept;

// Bounds of how far dissect() moves the bed, its gullies cut and crests left standing, anywhere
// within `within` metres of `metres`, its walls standing no steeper than `steepest`.
[[nodiscard]] Interval gully_bounds(DVec3 metres, double within, double steepest,
                                    const Gullies& gullies, double lod) noexcept;

} // namespace hog::globe
