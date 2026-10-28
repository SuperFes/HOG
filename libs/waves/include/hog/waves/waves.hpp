#pragma once

#include <hog/core/math.hpp>
#include <hog/core/time.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <numbers>
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
// The most the water may bunch along the ground, of how far it would to fold the surface over:
// where the waves would bunch it more, they swing it along the ground that much less. As
// shaders/waves.slang has it.
inline constexpr double most_bunching = 0.8;

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

// One system of waves at sea: the wind sea a wind is raising, or a swell run on from a wind
// elsewhere. Its variance runs by frequency as JONSWAP's spectrum, and by heading as a power of
// the cosine off its own.
struct System {
    DVec3 heading{1.0, 0.0, 0.0}; // unit, along the ground, in the planet's frame
    double height = 0.0;          // significant, metres
    double peak = 1.0;            // rad/s
    double peakedness = 1.0;      // JONSWAP's γ; 1 is Pierson and Moskowitz's
    double spread = 2.0;          // the cosine's power
};

// As waves are measured: a sea still growing under its wind peaks more sharply than one grown
// full, and a swell, sorted by speed over the distance it ran, more sharply still and narrower.
inline constexpr double wind_sea_peakedness = 3.3;
inline constexpr double swell_peakedness = 7.0;
inline constexpr double swell_spread = 8.0;

// The sea a wind ten metres up raises, blowing long and far enough: Pierson and Moskowitz's.
// Precondition: the wind lies along the ground.
[[nodiscard]] System grown_sea(DVec3 wind);

// JONSWAP's seas, grown over X metres of a wind of U ten metres up: their variance·g²/U⁴ is
// fetch_growth·χ and their peak·U/g fetch_peak_scale·χ^-fetch_peak_power, with χ = gX/U².
inline constexpr double fetch_growth = 1.6e-7;
inline constexpr double fetch_peak_scale = 2.0 * std::numbers::pi * 3.5;
inline constexpr double fetch_peak_power = 0.33;

// How high a train of `frequency` rad/s runs where a wind of `speed` m/s ten metres up has blown
// over only `fetch` metres of water, against how high it runs in grown_sea's: the sea there has
// the shape of grown_sea's spectrum, at the height and peak JONSWAP's laws give, so its longer
// waves are missing and its shorter ones steeper, until it has grown as full.
[[nodiscard]] double fetched(double frequency, double fetch, double speed) noexcept;
// How many times faster the peak of a sea grown so turns than that of grown_sea's: 1 once grown
// full, and none without water or wind.
[[nodiscard]] double fetched_peak(double fetch, double speed) noexcept;

// The sea somewhere: its systems; the current under them, which steepens the waves it runs
// against and stretches those it runs with; and the wind over them, which breaks them.
struct SeaState {
    std::vector<System> systems;
    DVec3 current{0.0}; // m/s, along the ground, in the planet's frame
    DVec3 wind{0.0};    // m/s ten metres up, likewise
};

// The system's variance falling in each band, m², summing to its height squared over 16.
[[nodiscard]] std::array<double, bands> band_variances(const System& system);
// How its variance spreads by heading, per radian, for a heading whose cosine off its own is
// `along`.
[[nodiscard]] double spreading(const System& system, double along);
// How much higher a wave runs, at `speed` m/s on still water, over a current of `current` m/s
// along its heading: none at all once the current is too strong for it to run against.
[[nodiscard]] double steepened(double speed, double current);

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

// The sea's floor under a point: how deep the water over it is at rest, and which way, along the
// ground, and how fast it deepens, metres per metre; and how far out from the waterline it lies,
// and which way out, unit along the ground. The waves' phases lag as over a sandy shore's profile,
// as waves/shoaling.hpp has it, as far out as the point lies, or, where that's unknown, as deep.
struct Floor {
    double depth = std::numeric_limits<double>::infinity();
    DVec3 deepening{0.0};
    double out = std::numeric_limits<double>::infinity();
    DVec3 seaward{0.0};
};

// At `rest`, a point of sea level in the planet's frame, over `floor`: in shallow water the waves
// that run towards the shore shorten, turn towards it and rise until they break, and the rest die
// away.
[[nodiscard]] Surface surface_at(std::span<const Train> trains, std::span<const Wave> waves,
                                 DVec3 rest, GameTime time, const Floor& floor = {});

// Metres above sea level of the water's surface over or under `point`, on a planet whose sea
// level lies `radius` from its centre, over `floor`.
[[nodiscard]] double height_at(std::span<const Train> trains, std::span<const Wave> waves,
                               DVec3 point, double radius, GameTime time, const Floor& floor = {});

// At most `count` of the waves, the strongest of each band in turn, longest first; and the mean
// square slope of those left out.
struct Strongest {
    std::vector<Wave> waves;
    double slope_left = 0.0;
};

[[nodiscard]] Strongest strongest(std::span<const Train> trains, std::span<const Wave> waves,
                                  std::size_t count);

} // namespace hog::waves
