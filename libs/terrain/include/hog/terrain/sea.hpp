#pragma once

#include <hog/core/math.hpp>
#include <hog/terrain/bend.hpp>

#include <cstdint>
#include <vector>

namespace hog::terrain {

// A corner of the sea's disc, which lies round the point under the eye out to the horizon.
struct SeaVertex {
    float out = 0.0F;         // from 0 under the eye to 1 at the rim
    Vec2 towards{1.0F, 0.0F}; // unit, along the ground: east and north
};

// Rings round a centre, joined by spokes; triangles three indices each, counter-clockwise seen from
// above.
struct SeaDisc {
    std::vector<SeaVertex> vertices;
    std::vector<std::uint16_t> indices;
};

[[nodiscard]] SeaDisc sea_disc(int rings, int spokes);

// How far the disc's rings reach along the ground, in metres: they start about `nearest` apart
// and spread in proportion to their distance, out to `furthest`.
struct SeaReach {
    double nearest = 1.0;
    double furthest = 1.0;
};

// Out to just past the horizon of sea level as it's drawn, starting as far apart as the eye stands
// over the water nearest below it, `water` metres above sea level.
[[nodiscard]] SeaReach sea_reach(const Bend& bend, double water = 0.0);

// Metres along the ground from under the eye to the ring `out` of the way to the rim.
[[nodiscard]] double sea_along(const SeaReach& reach, double out);

// Where sea level `along` metres from under the eye, in the unit direction `towards` along the
// ground there, is drawn: as bent puts it, relative to the eye.
[[nodiscard]] DVec3 sea_from_eye(const Bend& bend, double along, DVec3 towards);

// Seawater as shaders/water.slang draws it; change both together. Schlick's reflectance looking
// straight down, the clearest colour's absorption per metre, and the most glancing a view down
// through the water is taken as, for the length of its path there.
inline constexpr double water_facing_reflection = 0.02;
inline constexpr double water_clearest_absorbing = 0.03;
inline constexpr double water_least_facing = 0.1;

// The most that ground under the sea can show through it: what the sea's surface lets through, as
// Fresnel has it, times what of the clearest colour the water passes down to the ground and back
// up. Seen from `eye` metres from the centre of a sea `sea` metres in radius, of ground between
// `deepest` and `shallowest` metres from the centre, `nearest` to `farthest` radians round from
// under the eye. 1 for ground that isn't under the sea, or an eye that isn't above it.
[[nodiscard]] double seabed_showing(double eye, double sea, double deepest, double shallowest,
                                    double nearest, double farthest);

} // namespace hog::terrain
