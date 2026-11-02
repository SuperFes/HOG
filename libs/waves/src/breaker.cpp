#include <hog/waves/breaker.hpp>

#include <algorithm>
#include <cmath>

namespace hog::waves {
namespace {

// The shape of the curl, in units of the wave's amplitude: the lip's push ahead is a bump over the
// crest and the first of its front face, and its tip drops a little further down the face.
constexpr double push = 3.4;
constexpr double push_centre = 0.15;
constexpr double push_width = 0.6;
constexpr double drop = 0.5;
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

Curl curl_at(double phase, double stage, double amplitude) noexcept {
    const double s = std::clamp(stage, 0.0, 1.0);
    if (s <= 0.0 || amplitude <= 0.0) {
        return {};
    }
    const double ahead = (phase - push_centre) / push_width;
    const double down = (phase - drop_centre) / drop_width;
    const double bump = std::exp(-ahead * ahead);
    const double dip = std::exp(-down * down);
    const double push_scale = push * s * amplitude;
    const double drop_scale = drop * s * amplitude;
    return {.along = push_scale * bump,
            .up = -drop_scale * dip,
            .along_rate = push_scale * bump * (-2.0 * ahead / push_width),
            .up_rate = drop_scale * dip * (2.0 * down / drop_width)};
}

double breaking_stage(double height, double depth) noexcept {
    if (!(depth > 0.0) || !(height > 0.0)) {
        return 0.0;
    }
    const double ratio = height / (holds * depth);
    return std::clamp((ratio - 1.0) / fully_over, 0.0, 1.0);
}

double plunging(double slope, double steepness) noexcept {
    if (!(slope > 0.0) || !(steepness > 0.0)) {
        return 0.0;
    }
    const double iribarren = slope / std::sqrt(steepness);
    return smooth(plunges_from - edge, plunges_from + edge, iribarren) *
           (1.0 - smooth(surges_from - edge, surges_from + edge, iribarren));
}

} // namespace hog::waves
