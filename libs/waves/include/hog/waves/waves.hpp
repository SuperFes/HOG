#pragma once

#include <hog/core/math.hpp>
#include <hog/core/time.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace hog::waves {

inline constexpr double gravity = 9.81; // m/s²

// The planet's wave trains fall into bands of frequency, each the same ratio wider than the last,
// from swell long enough to cross an ocean to ripples a hand across.
inline constexpr int bands = 16;
inline constexpr int trains_per_band = 40;
inline constexpr double lowest_frequency = 0.25; // rad/s
inline constexpr double highest_frequency = 14.0;

// A train shows fully where it runs within this sine of the ground, fading out by the next.
inline constexpr double fully_showing = 0.2;
inline constexpr double last_showing = 0.35;

// How much of its orbit's width the water moves along a wave as well as up and down: all of it, as
// water truly moves. As shaders/waves.slang has it.
inline constexpr double choppiness = 1.0;

// One of the planet's wave trains: a plane wave through its whole volume, which shows on the
// surface where it runs nearly along it. The planet has the same trains for all time; the sea
// only sets how high each runs.
struct Train {
    DVec3 heading{1.0, 0.0, 0.0}; // unit, in the planet's frame
    double wavenumber = 0.0;      // rad/m
    double frequency = 0.0;       // rad/s, deep water's for the wavenumber
    double phase = 0.0;           // rad, through the planet's centre at the Unix epoch
    int band = 0;
};

// The planet's trains, band by band, fixed by its seed.
[[nodiscard]] std::vector<Train> planet_trains(std::uint64_t seed);

// The bounds of a band's frequencies, rad/s.
[[nodiscard]] double band_lowest(int band) noexcept;
[[nodiscard]] double band_width(int band) noexcept;

// How much of a train shows, by its amplitude, where it runs `rising` (the sine) off the ground.
[[nodiscard]] double showing(double rising) noexcept;

// The sea somewhere: as the wind over it, ten metres up, would raise it, had it blown long and
// far enough.
struct SeaState {
    DVec3 wind{0.0}; // m/s, along the ground, in the planet's frame
};

// The sea's variance by frequency, m²·s/rad: Pierson and Moskowitz's.
[[nodiscard]] double spectrum(const SeaState& sea, double frequency);
// How that variance spreads by heading, per radian, for a heading `across` radians off the wind.
[[nodiscard]] double spreading(double across);

// A train as it runs somewhere.
struct Wave {
    std::uint32_t train = 0;
    double amplitude = 0.0; // metres, where the train fully shows
};

// The trains showing at the unit direction `up` under `sea`, at least `least` metres high.
[[nodiscard]] std::vector<Wave> waves_at(std::span<const Train> trains, DVec3 up,
                                         const SeaState& sea, double least = 0.001);

// The train's phase at a point, in the planet's frame, at `time`: within ±π.
[[nodiscard]] double phase_at(const Train& train, DVec3 point, GameTime time);

// Of the water at a point of sea level, where the waves move it and how its surface faces there.
struct Surface {
    DVec3 moved{0.0};
    DVec3 normal{0.0, 0.0, 1.0};
};

// At `rest`, a point of sea level in the planet's frame.
[[nodiscard]] Surface surface_at(std::span<const Train> trains, std::span<const Wave> waves,
                                 DVec3 rest, GameTime time);

// Metres above sea level of the water's surface over or under `point`, on a planet whose sea
// level lies `radius` from its centre.
[[nodiscard]] double height_at(std::span<const Train> trains, std::span<const Wave> waves,
                               DVec3 point, double radius, GameTime time);

// At most `count` of the waves, the strongest of each band in turn, longest first; and the mean
// square slope of those left out.
struct Strongest {
    std::vector<Wave> waves;
    double slope_left = 0.0;
};

[[nodiscard]] Strongest strongest(std::span<const Train> trains, std::span<const Wave> waves,
                                  std::size_t count);

} // namespace hog::waves
