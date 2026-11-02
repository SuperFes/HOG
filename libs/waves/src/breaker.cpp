#include <hog/waves/breaker.hpp>

#include <algorithm>
#include <cmath>
#include <numbers>

namespace hog::waves {
namespace {

// The shape of the curl: the front face is drawn back under the crest by this many radians of
// phase, as much as its sine and fading in over the crest; the lip's tip drops a little further
// down the face, in units of the wave's amplitude.
constexpr double draw_back = 1.7;
constexpr double draw_from = -0.3;
constexpr double draw_to = 0.5;
constexpr double drop = 0.6;
constexpr double drop_centre = 0.55;
constexpr double drop_width = 0.45;

// Waves as high as the depth holds break at this ratio of height to depth, and curl over fully by
// this much more.
constexpr double holds = 0.42;
constexpr double fully_over = 0.6;

// Iribarren's bounds: below, waves spill; above, they plunge, until they surge.
constexpr double plunges_from = 0.5;
constexpr double surges_from = 3.3;
constexpr double edge = 0.15;

[[nodiscard]] double smooth(double from, double to, double x) noexcept {
    const double t = std::clamp((x - from) / (to - from), 0.0, 1.0);
    return t * t * (3.0 - (2.0 * t));
}

} // namespace

Curl curl_at(double turned, double stage, double amplitude, double wavenumber) noexcept {
    // The phase within a turn of the crest, as the curl is shaped.
    const double phase = std::remainder(turned, 2.0 * std::numbers::pi);
    const double s = std::clamp(stage, 0.0, 1.0);
    if (s <= 0.0 || !(amplitude > 0.0) || !(wavenumber > 0.0)) {
        return {};
    }
    // Over the front face the curl fades in over the crest and is gone at the foot.
    const double t = std::clamp((phase - draw_from) / (draw_to - draw_from), 0.0, 1.0);
    const double fade = t * t * (3.0 - (2.0 * t));
    const double fade_rate = t > 0.0 && t < 1.0 ? 6.0 * t * (1.0 - t) / (draw_to - draw_from) : 0.0;
    const double scale = s * draw_back / wavenumber;
    const double down = (phase - drop_centre) / drop_width;
    const double dip = std::exp(-down * down);
    const double drop_scale = drop * s * amplitude;
    return {.along = -scale * std::sin(phase) * fade,
            .up = -drop_scale * dip,
            .along_rate = -scale * ((std::cos(phase) * fade) + (std::sin(phase) * fade_rate)),
            .up_rate = drop_scale * dip * (2.0 * down / drop_width)};
}

double breaking_stage(double height, double depth) noexcept {
    if (!(depth > 0.0) || !(height > 0.0)) {
        return 0.0;
    }
    const double ratio = height / (holds * depth);
    return std::clamp((ratio - 1.0) / fully_over, 0.0, 1.0);
}

double plunging(double slope, double offshore_steepness) noexcept {
    if (!(slope > 0.0) || !(offshore_steepness > 0.0)) {
        return 0.0;
    }
    const double iribarren = slope / std::sqrt(offshore_steepness);
    return smooth(plunges_from - edge, plunges_from + edge, iribarren) *
           (1.0 - smooth(surges_from - edge, surges_from + edge, iribarren));
}

} // namespace hog::waves
