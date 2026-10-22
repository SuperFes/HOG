#pragma once

#include <hog/core/math.hpp>

#include <cstdint>
#include <optional>

namespace hog::render {

// Nothing's kept on a grid over a longer step, or once the eye is further than this from where its
// plane touches sea level, where the plane strays from it.
inline constexpr double grid_longest_step = 2.0;    // seconds
inline constexpr double grid_plane_reach = 2'000.0; // metres

// How a grid moved since the step before.
struct GridStep {
    // What lay at texel t + shift now lies at t.
    glm::ivec2 shift{0};
    double seconds = 0.0;
    // Whether anything kept before is kept on.
    bool kept = false;
};

// Where a grid lies from the eye: where its plane touches sea level, the ways its rows and columns
// run along it, and its first texel's corner, metres along those from where it touches; how far
// the water's drifted along them since it was laid; and how far apart its texels are, metres.
struct GridPlacing {
    DVec3 anchor{0.0};
    DVec3 east{1.0, 0.0, 0.0};
    DVec3 north{0.0, 1.0, 0.0};
    DVec2 corner{0.0};
    DVec2 drifted{0.0};
    double spacing = 1.0;
};

// A grid of `size` texels a side, `spacing` metres apart, kept round the eye on a plane touching
// sea level near it. The grid drifts with the water at the surface, so what's kept on it needn't
// move, and steps a whole texel at a time to stay centred under the eye.
class DriftingGrid {
public:
    DriftingGrid(std::uint32_t size, double spacing) noexcept : size_{size}, spacing_{spacing} {}

    // Moves the grid on to an eye over sea level at `up` on a planet of `radius`, `seconds` after
    // the step before, the water at the surface drifting at `drift`, m/s.
    GridStep follow(DVec3 up, double radius, DVec3 drift, double seconds);
    // Keeps nothing on the next step, as where what's kept was left over a sea not as it is.
    void forget() noexcept { plane_.reset(); }
    // Where it lies from an eye `height` metres above sea level at `up`.
    [[nodiscard]] GridPlacing placing(DVec3 up, double radius, double height) const;

private:
    struct Plane {
        DVec3 touching;
        DVec3 east;
        DVec3 north;
    };
    std::uint32_t size_;
    double spacing_;
    std::optional<Plane> plane_;
    // How far the water's drifted along the plane since it was laid, metres.
    DVec2 drifted_{0.0};
    // The texel the grid's centred on, counted from where the plane touches, as the water's
    // drifted.
    glm::ivec2 centre_{0};
};

// Where `rest`, a point of sea level from the eye, lies on the grid as `placing` lays it, in
// texels from its first's corner.
[[nodiscard]] DVec2 grid_texel(const GridPlacing& placing, DVec3 rest) noexcept;

} // namespace hog::render
