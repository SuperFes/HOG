#pragma once

#include <hog/core/math.hpp>

#include <limits>
#include <span>
#include <vector>

namespace hog::atmosphere {

// Air as Hillaire's "A Scalable and Production Ready Sky and Atmosphere Rendering Technique" gives
// Earth's, in metres and per metre. The three channels are red, green and blue light.
struct Air {
    double bottom = 6'360'000.0; // the ground's radius
    double top = 6'460'000.0;    // above which there's no air
    DVec3 rayleigh_scattering{5.802e-6, 13.558e-6, 33.1e-6};
    double rayleigh_height = 8'000.0; // over which it thins by e
    double mie_scattering = 3.996e-6;
    double mie_extinction = 4.40e-6;
    double mie_height = 1'200.0;
    double mie_asymmetry = 0.8;
    DVec3 ozone_absorption{0.650e-6, 1.881e-6, 0.085e-6};
    double ozone_peak = 25'000.0;       // height at which ozone is densest
    double ozone_half_width = 15'000.0; // thinning to none this far either side
    DVec3 ground_albedo{0.3};

    // The same air on ground of `radius`: heights in proportion and coefficients in inverse, so
    // light fares the same through it.
    [[nodiscard]] Air scaled_to(double radius) const noexcept;
};

// Scattering and extinction at a height, per metre.
struct Medium {
    DVec3 rayleigh{0.0};
    double mie = 0.0;
    DVec3 extinction{0.0};

    [[nodiscard]] DVec3 scattering() const noexcept { return rayleigh + DVec3{mie}; }
};

[[nodiscard]] Medium medium_at(const Air& air, double height) noexcept;

// Along a ray from `height` at `mu`, the cosine of its angle from straight up, to where it leaves
// the air or meets the ground, marched finely.
[[nodiscard]] DVec3 optical_depth(const Air& air, double height, double mu);

[[nodiscard]] double rayleigh_phase(double cos_angle) noexcept;
[[nodiscard]] double mie_phase(double cos_angle, double asymmetry) noexcept;

// Light scattered towards the eye along a ray, and the share of what lies beyond that still
// reaches it.
struct Scattered {
    DVec3 light{0.0}; // radiance, for sunlight of irradiance 1
    DVec3 transmittance{1.0};
};

inline constexpr int transmittance_width = 256; // across directions
inline constexpr int transmittance_height = 64; // across heights
inline constexpr int scattering_size = 32;      // across both

// Where in the transmittance table, from 0 to 1 across texel centres, a ray from `radius` at `mu`
// lies; for rays that don't meet the ground.
[[nodiscard]] DVec2 transmittance_uv(const Air& air, double radius, double mu) noexcept;
// Where in the multiple-scattering table a point at `height` with the sun at `sun_mu` lies.
[[nodiscard]] DVec2 scattering_uv(const Air& air, double height, double sun_mu) noexcept;

// Air with its transmittance and multiple scattering tabulated, as the GPU tabulates them.
class Model {
public:
    explicit Model(const Air& air = {});

    [[nodiscard]] const Air& air() const noexcept { return air_; }

    // The share of light from the top of the air reaching `height` along `mu`; none where the
    // ground is in the way.
    [[nodiscard]] DVec3 transmittance(double height, double mu) const noexcept;
    // Light scattered more than once towards any direction at `height`, with the sun at `sun_mu`,
    // per unit of scattering.
    [[nodiscard]] DVec3 multiple_scattering(double height, double sun_mu) const noexcept;

    // Along `view` from the eye at `height`, with `sun` towards the sun, both unit in a frame in
    // which up from the eye is +Z: to where the ray leaves the air or meets the ground, or to
    // `distance` if that's nearer.
    [[nodiscard]] Scattered march(double height, DVec3 view, DVec3 sun,
                                  double distance = std::numeric_limits<double>::infinity(),
                                  int steps = 64) const noexcept;

    // Irradiance from the sky, but not the sun's disc, on level ground at `height`, with `sun`
    // as for march().
    [[nodiscard]] DVec3 sky_irradiance(double height, DVec3 sun) const noexcept;

    // Row by row from the ground up, each across directions from the sun straight below.
    [[nodiscard]] std::span<const DVec3> transmittance_table() const noexcept {
        return transmittance_;
    }
    [[nodiscard]] std::span<const DVec3> scattering_table() const noexcept { return scattering_; }

private:
    Air air_;
    std::vector<DVec3> transmittance_;
    std::vector<DVec3> scattering_;
};

} // namespace hog::atmosphere
