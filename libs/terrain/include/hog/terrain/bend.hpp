#pragma once

#include <hog/core/math.hpp>

namespace hog::terrain {

struct BendSettings {
    // What the ground is drawn curving like near it: Earth's.
    double faked_radius = 6'371'000.0;
    // Metres of the eye over the ground by which drawing curves truly again: low enough that the
    // far side of a small planet, folded around the faked one's rim, stays below the horizon.
    double blended_out = 10'000.0;
};

// How drawing bends the planet: onto a sphere of a faked radius touching the true one under the
// eye, each point as far along the ground from there and as high above sea level as it truly is.
// The eye stays put, and points are taken and given relative to it, for precision near it.
struct Bend {
    DVec3 up{0.0, 0.0, 1.0}; // unit, from the planet's centre through the eye
    double eye_height = 0.0; // above sea level
    double radius = 1.0;
    double faked = 1.0;
};

// The faked radius seen from `over_ground` metres above the ground, blending its curvature to the
// true one's; the true radius once blended out, or if it's the larger.
[[nodiscard]] double faked_radius(double radius, double over_ground,
                                  const BendSettings& settings = {});

// Seen from `eye`, over ground `ground` metres above sea level.
[[nodiscard]] Bend bend_from(WorldPosition eye, double radius, double ground,
                             const BendSettings& settings = {});

[[nodiscard]] DVec3 bent(const Bend& bend, DVec3 from_eye);
// Where a point drawn at `from_eye` truly is, within the faked sphere's cap the planet bends into.
[[nodiscard]] DVec3 unbent(const Bend& bend, DVec3 from_eye);

} // namespace hog::terrain
