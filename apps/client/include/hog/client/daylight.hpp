#pragma once

#include <hog/atmosphere/model.hpp>
#include <hog/core/math.hpp>
#include <hog/core/time.hpp>
#include <hog/render/clouds.hpp>
#include <hog/sim/calendar.hpp>

#include <optional>

namespace hog::client {

// Planet seconds a real one at most, so a four-hour day passes in about a second.
inline constexpr double fastest_time = 16'384.0;

// Planet time as the client keeps it, running at a rate of planet seconds to real ones.
class PlanetClock {
public:
    explicit PlanetClock(GameTime start) noexcept : time_(start) {}

    [[nodiscard]] GameTime time() const noexcept { return time_; }
    [[nodiscard]] double rate() const noexcept { return rate_; }

    void advance(double seconds) noexcept;
    // Doubling or halving the rate, from 1 up to fastest_time.
    void faster() noexcept;
    void slower() noexcept;

private:
    GameTime time_;
    double rate_ = 1.0;
    double carried_ = 0.0; // planet microseconds too few to tick over yet
};

// `now`, or the soonest time after it that it's `time_of_day` where `up` is.
[[nodiscard]] GameTime start_time(const sim::Calendar& calendar, GameTime now, DVec3 up,
                                  std::optional<double> time_of_day);

// Irradiance on level ground at the eye.
struct EyeLight {
    DVec3 sun{0.0};
    // With a faint glow at night, from the moon and stars still to come, so the dark isn't black.
    DVec3 sky{0.0};
};

// At `height` of the air's metres, with the sun at `sun_mu`, the cosine of its angle from straight
// up.
[[nodiscard]] EyeLight light_at_eye(const atmosphere::Model& model, double height, double sun_mu);

// The light under cloud letting `shade` of it through: the sun's direct share, and the sun's and
// sky's scattered through lighting the ground as the sky does.
[[nodiscard]] EyeLight under_cloud(const EyeLight& light, render::CloudShade shade) noexcept;

// How much brighter the picture is than the light at the eye: adapting partly to it, so night is
// darker than day but can still be seen in.
[[nodiscard]] double exposure_for(const EyeLight& light) noexcept;

// From an exposure towards another over `seconds`, as eyes adapt.
[[nodiscard]] double adapted(double from, double to, double seconds) noexcept;

} // namespace hog::client
