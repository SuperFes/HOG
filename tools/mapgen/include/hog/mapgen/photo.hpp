#pragma once

#include <hog/mapgen/colour.hpp>
#include <hog/planet/climate.hpp>
#include <hog/planet/tangent_frame.hpp>

#include <cstdint>
#include <span>

namespace hog::mapgen {

// What lies at a point, in the terms a camera sees it by.
struct Surface {
    planet::Biome biome = planet::Biome::Grassland; // Ocean for the sea, Lake for a lake
    double depth = 0.0;                             // metres of sea
    double height = 0.0;                            // metres the ground stands above the sea
    double ice = 0.0;                               // share of the sea iced over, 0 to 1
    double plants = 1.0;                            // share grown of what the biome holds, 0 to 1
    double fire = 0.0;                              // share alight, 0 to 1
    double soil = 0.0;                              // how full the soil is, 0 to 1
    double snow = 0.0;                              // mm of water lying as snow
    double river = 0.0; // share of the point a river covers at its usual flow, 0 to 1
    double flow = 1.0;  // the river's flow over its usual

    friend bool operator==(const Surface&, const Surface&) = default;
};

// The share of the light the surface reflects, as it would look from above in daylight.
[[nodiscard]] LinearRgb true_colour(const Surface& surface) noexcept;

// How the sun lights a photo.
struct Lighting {
    double relief = 4.0;         // how many times over the heights are drawn
    double sun_elevation = 35.0; // degrees above the horizon
    double sun_azimuth = 315.0;  // degrees clockwise from north that the light comes from
    // The sun where the calendar has it at the photo's time instead, with night where it has set.
    bool real_sun = false;
    double sky = 0.25;             // light from the open sky, against the sun's 1 overhead
    double cloud_height = 3'000.0; // metres up that clouds cast their shadows from
    double glint = 0.5;            // the sun's glare off open water at its brightest
    int shadow_reach = 32;         // pixels a shadow is looked for towards the sun

    friend bool operator==(const Lighting&, const Lighting&) = default;
};

// Towards the lighting's sun, as seen from a point on the surface.
[[nodiscard]] DVec3 sun_towards(const Lighting& lighting,
                                const planet::TangentFrame& frame) noexcept;

// The normal of a surface through points either side of one, across and down, facing `up`.
[[nodiscard]] DVec3 surface_normal(DVec3 left, DVec3 right, DVec3 below, DVec3 above,
                                   DVec3 up) noexcept;

// Light falling on a surface: the share `sunlit` of the sun's that isn't shaded, and the share of
// the sky it faces, both fading through twilight, with a glimmer left at night.
[[nodiscard]] double light(DVec3 normal, DVec3 up, DVec3 sun, double sky, double sunlit) noexcept;

// Light on flat open ground, which a photo is exposed for: under the lighting's sun, or under the
// sun overhead for the real one.
[[nodiscard]] double exposure(const Lighting& lighting) noexcept;

// Where the sun's ray up from a unit direction on the ground meets clouds `height` metres up, as
// a unit direction; held within ten times the height when the sun is low.
[[nodiscard]] DVec3 towards_clouds(DVec3 point, DVec3 sun, double height,
                                   double planet_radius) noexcept;

// Glare off open water seen from straight above, from 1 with the sun overhead to 0 away from it.
[[nodiscard]] double sea_glint(DVec3 up, DVec3 sun) noexcept;

// The share of a surface that's open water, to glint.
[[nodiscard]] double open_water(const Surface& surface) noexcept;

// Noise for where cloud gathers and where it parts, about [-1, 1] and near normal, across `across`
// metres of sky a lattice cell.
[[nodiscard]] double cloud_noise(std::uint64_t seed, DVec3 metres, double across) noexcept;

// Cloud over a point.
struct Cloud {
    double shape = 0.0; // 1 under cloud, fading to 0 across its edge
    double depth = 0.0; // how far into it, from 0 at its edge to 1 deep in

    // Thin at its edges, so the ground shows through.
    [[nodiscard]] double opacity() const noexcept;

    friend bool operator==(const Cloud&, const Cloud&) = default;
};

// The cloud where its noise is `noise`, with `cover` of the sky about there clouded: the noise
// picks out that share of the ground to lie under it.
[[nodiscard]] Cloud cloud_over(double cover, double noise) noexcept;

// Cloud tops in daylight: greyer where thin, whiter deep in and where storms tower (0 to 1).
[[nodiscard]] LinearRgb cloud_colour(const Cloud& cloud, double storm) noexcept;

// Heights in metres, row by row.
struct HeightField {
    int width = 0;
    int height = 0;
    std::span<const double> heights;
};

// Whether higher ground towards the sun shades (x, y): followed `steps` times by `step` pixels,
// while the sun's ray climbs `rise` metres a step.
[[nodiscard]] bool in_shadow(const HeightField& field, int x, int y, DVec2 step, double rise,
                             int steps) noexcept;

} // namespace hog::mapgen
