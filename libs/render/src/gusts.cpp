#include <hog/render/gusts.hpp>

#include <cmath>

namespace hog::render {

namespace {

// Of the wind near the ground, how fast the gusts are carried along it, and how fast through the
// ground, turning their shapes over a few dozen metres of travel.
constexpr double carried = 1.5;
constexpr double turning = 0.4;

} // namespace

DVec3 gust_drift(DVec3 wind, DVec3 up) noexcept {
    return (wind * carried) + (up * (glm::length(wind) * turning));
}

DVec3 gust_anchor(const WorldPosition& eye, DVec3 drifted) noexcept {
    const auto within = [](double metres) {
        const double kept = std::fmod(metres, gust_period);
        return kept < 0.0 ? kept + gust_period : kept;
    };
    const DVec3 from = eye.metres() - drifted;
    return {within(from.x), within(from.y), within(from.z)};
}

} // namespace hog::render
