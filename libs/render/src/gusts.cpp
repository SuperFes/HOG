#include <hog/render/gusts.hpp>

#include <cmath>
#include <numbers>

namespace hog::render {

namespace {

// Of the wind near the ground, how fast the gusts are carried along it, and how fast through the
// field's up, turning their shapes over a few dozen metres of travel.
constexpr double carried = 1.5;
constexpr double turning = 0.25;
// Seconds the field's axes take to follow most of the way round to a new wind.
constexpr double veering = 20.0;

double within(double kept, double period) noexcept {
    kept = std::fmod(kept, period);
    return kept < 0.0 ? kept + period : kept;
}

// Any level direction, where none is known.
DVec3 level_of(DVec3 up) noexcept {
    const DVec3 side = std::abs(up.z) < 0.9 ? DVec3{0.0, 0.0, 1.0} : DVec3{1.0, 0.0, 0.0};
    return glm::normalize(glm::cross(side, up));
}

} // namespace

DVec3 gust_anchor(const WorldPosition& eye, DVec3 drifted) noexcept {
    const DVec3 from = eye.metres() - drifted;
    return {within(from.x, gust_period), within(from.y, gust_period), within(from.z, gust_period)};
}

void Gusts::blow(const WorldPosition& eye, DVec3 wind, DVec3 up, double seconds) noexcept {
    const DVec3 level = wind - (up * glm::dot(wind, up));
    const double speed = glm::length(level);
    DVec3 along = seen_.along - (up * glm::dot(seen_.along, up));
    if (speed > 1e-6) {
        const DVec3 toward = level / speed;
        along = blown_ ? glm::mix(along, toward, 1.0 - std::exp(-seconds / veering)) : toward;
        blown_ = true;
    }
    along = glm::length(along) > 1e-6 ? glm::normalize(along) : level_of(up);
    const DVec3 across = glm::cross(up, along);

    const DVec3 moved = eye_ ? eye - *eye_ : DVec3{0.0};
    eye_ = eye;
    const DVec3 drift{carried * speed / gust_stretch, 0.0, turning * speed};
    const DVec3 anchor =
        seen_.anchor +
        DVec3{glm::dot(moved, along) / gust_stretch, glm::dot(moved, across), glm::dot(moved, up)} -
        (drift * seconds);
    constexpr double wavenumber = 2.0 * std::numbers::pi / sway_wavelength;
    const double wave = seen_.wave + (wavenumber * glm::dot(moved, along)) -
                        (wavenumber * carried * speed * seconds);
    seen_ = {.anchor = {within(anchor.x, gust_period), within(anchor.y, gust_period),
                        within(anchor.z, gust_period)},
             .along = along,
             .wave = within(wave, 2.0 * std::numbers::pi),
             .wind = glm::length(wind)};
}

} // namespace hog::render
