#pragma once

#include <hog/core/math.hpp>

#include <optional>

namespace hog::render {

// Gusts are patches of stronger wind drifting over the ground and changing shape as they go, a
// field gusts.slang reads in axes of its own: downwind, across the wind and up.

// Metres the gusts' field repeats over, along each of its axes, as gusts.slang's.
inline constexpr double gust_period = 2'048.0;

// How many times longer the gusts' patches are downwind than across it, as gusts.slang's.
inline constexpr double gust_stretch = 2.5;

// Metres from crest to crest of the waves running downwind through swaying grass, as
// gusts.slang's.
inline constexpr double sway_wavelength = 12.0;

// Where in the planet's frame the fields fixed to the ground lie from `eye`, within their period,
// so their precision is kept.
[[nodiscard]] DVec3 gust_anchor(const WorldPosition& eye, DVec3 drifted) noexcept;

// The gusts as a frame draws them.
struct GustsSeen {
    DVec3 anchor{0.0}; // where the field lies from the eye, in its own axes, within its period
    DVec3 along{1.0, 0.0, 0.0}; // downwind, level
    double wave = 0.0;          // radians into its sway grass at the eye is, within a turn
    double wind = 0.0;          // m/s the wind blows near the ground on the whole
};

// The gusts over the ground, carried downwind faster than the wind blows there, as they're brought
// down from the faster air above, and turning their shapes over as they go; their axes following
// the wind round as it veers, slowly, so the field never swings about.
class Gusts {
public:
    // The eye moved to `eye`, where `up` is up, and the wind near the ground blew `wind` for
    // `seconds`.
    void blow(const WorldPosition& eye, DVec3 wind, DVec3 up, double seconds) noexcept;

    [[nodiscard]] const GustsSeen& seen() const noexcept { return seen_; }

private:
    std::optional<WorldPosition> eye_;
    bool blown_ = false;
    GustsSeen seen_;
};

} // namespace hog::render
