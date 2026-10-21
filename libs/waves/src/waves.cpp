#include <hog/core/hash.hpp>
#include <hog/waves/waves.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <numbers>

namespace hog::waves {

namespace {

constexpr double tau = 2.0 * std::numbers::pi;

// Pierson and Moskowitz's, for the wind 19.5 m up, which blows a little faster than at 10.
constexpr double pm_alpha = 8.1e-3;
constexpr double pm_beta = 0.74;
constexpr double wind_19_5 = 1.026;

const double band_ratio = std::pow(highest_frequency / lowest_frequency, 1.0 / bands);

double uniform(std::uint64_t seed, int band, int train, int which) noexcept {
    return static_cast<double>(hash_values(seed, band, train, which) >> 11U) * 0x1.0p-53;
}

// A rotation drawn evenly from all of them, as Shoemake has it.
DQuat turned(std::uint64_t seed, int band) {
    const double u1 = uniform(seed, band, -1, 0);
    const double u2 = uniform(seed, band, -1, 1);
    const double u3 = uniform(seed, band, -1, 2);
    const double a = std::sqrt(1.0 - u1);
    const double b = std::sqrt(u1);
    return DQuat{b * std::cos(tau * u3), a * std::sin(tau * u2), a * std::cos(tau * u2),
                 b * std::sin(tau * u3)};
}

double seconds_of(GameTime time) {
    return std::chrono::duration<double>(time.time_since_epoch()).count();
}

// Any two directions along the ground at `up`, making a right-handed frame with it.
std::array<DVec3, 2> across(DVec3 up) {
    const DVec3 other = std::abs(up.z) < 0.9 ? DVec3{0.0, 0.0, 1.0} : DVec3{1.0, 0.0, 0.0};
    const DVec3 first = glm::normalize(glm::cross(other, up));
    return {first, glm::cross(up, first)};
}

} // namespace

double band_lowest(int band) noexcept {
    return lowest_frequency * std::pow(band_ratio, band);
}

double band_width(int band) noexcept {
    return band_lowest(band) * (band_ratio - 1.0);
}

double showing(double rising) noexcept {
    const double off = (std::abs(rising) - fully_showing) / (last_showing - fully_showing);
    const double t = std::clamp(off, 0.0, 1.0);
    return 1.0 - (t * t * (3.0 - (2.0 * t)));
}

std::vector<Train> planet_trains(std::uint64_t seed) {
    std::vector<Train> trains;
    trains.reserve(static_cast<std::size_t>(bands) * trains_per_band);
    const double golden_angle = std::numbers::pi * (3.0 - std::sqrt(5.0));
    for (int band = 0; band < bands; ++band) {
        // Fibonacci's spiral, which spreads points evenly over a sphere, turned differently for
        // each band so their headings don't line up.
        const DQuat turn = turned(seed, band);
        for (int i = 0; i < trains_per_band; ++i) {
            const double z = 1.0 - (((2.0 * i) + 1.0) / trains_per_band);
            const double out = std::sqrt(1.0 - (z * z));
            const double around = golden_angle * i;
            const double frequency =
                band_lowest(band) * std::pow(band_ratio, uniform(seed, band, i, 0));
            trains.push_back({.heading = glm::normalize(
                                  turn * DVec3{out * std::cos(around), out * std::sin(around), z}),
                              .wavenumber = frequency * frequency / gravity,
                              .frequency = frequency,
                              .phase = tau * uniform(seed, band, i, 1),
                              .band = band});
        }
    }
    return trains;
}

double spectrum(const SeaState& sea, double frequency) {
    const double wind = glm::length(sea.wind) * wind_19_5;
    if (wind <= 0.0 || frequency <= 0.0) {
        return 0.0;
    }
    const double peak = gravity / wind;
    const double ratio = peak / frequency;
    return pm_alpha * gravity * gravity / std::pow(frequency, 5.0) *
           std::exp(-pm_beta * ratio * ratio * ratio * ratio);
}

double spreading(double across) {
    if (std::abs(across) >= std::numbers::pi / 2.0) {
        return 0.0;
    }
    const double along = std::cos(across);
    return 2.0 / std::numbers::pi * along * along;
}

std::vector<Wave> waves_at(std::span<const Train> trains, DVec3 up, const SeaState& sea,
                           double least) {
    std::vector<Wave> waves;
    const double wind = glm::length(sea.wind);
    if (wind <= 0.0) {
        return waves;
    }
    // How much each band shows here, weighed by how much each of its trains shows: about as much
    // anywhere, its headings being spread evenly over the sphere, but not quite.
    std::array<double, bands> shown{};
    for (const Train& train : trains) {
        shown.at(static_cast<std::size_t>(train.band)) += showing(glm::dot(up, train.heading));
    }
    for (std::size_t i = 0; i < trains.size(); ++i) {
        const Train& train = trains[i];
        const double rising = glm::dot(up, train.heading);
        if (showing(rising) <= 0.0) {
            continue;
        }
        const double per_radian = shown.at(static_cast<std::size_t>(train.band)) / tau;
        const DVec3 along = train.heading - (up * rising);
        const double off = std::acos(
            std::clamp(glm::dot(along, sea.wind) / (glm::length(along) * wind), -1.0, 1.0));
        const double variance =
            spectrum(sea, train.frequency) * spreading(off) * band_width(train.band) / per_radian;
        const double amplitude = std::sqrt(2.0 * variance);
        if (amplitude >= least) {
            waves.push_back({.train = static_cast<std::uint32_t>(i), .amplitude = amplitude});
        }
    }
    return waves;
}

double phase_at(const Train& train, DVec3 point, GameTime time) {
    return std::remainder((train.wavenumber * glm::dot(train.heading, point)) -
                              (train.frequency * seconds_of(time)) + train.phase,
                          tau);
}

Surface surface_at(std::span<const Train> trains, std::span<const Wave> waves, DVec3 rest,
                   GameTime time) {
    const DVec3 up = glm::normalize(rest);
    Surface surface;
    // How the surface slopes, and how its water bunches along the ground, against the ground
    // beneath at rest.
    DVec3 slope{0.0};
    glm::dmat3 bunching{0.0};
    for (const Wave& wave : waves) {
        const Train& train = trains[wave.train];
        const double rising = glm::dot(up, train.heading);
        const double shown = std::sqrt(showing(rising));
        if (shown <= 0.0) {
            continue;
        }
        const DVec3 along = train.heading - (up * rising);
        const double flat = glm::length(along);
        const DVec3 heading = along / flat;
        const double amplitude = wave.amplitude * shown;
        const double wavenumber = train.wavenumber * flat;
        const double phase = phase_at(train, rest, time);
        const double c = std::cos(phase);
        const double s = std::sin(phase);
        surface.moved += (up * (amplitude * c)) - (heading * (choppiness * amplitude * s));
        slope += heading * (amplitude * wavenumber * s);
        bunching += glm::outerProduct(heading, heading) * (choppiness * amplitude * wavenumber * c);
    }
    const auto [first, second] = across(up);
    const DVec3 one = first - (up * glm::dot(slope, first)) - (bunching * first);
    const DVec3 two = second - (up * glm::dot(slope, second)) - (bunching * second);
    surface.normal = glm::normalize(glm::cross(one, two));
    return surface;
}

double height_at(std::span<const Train> trains, std::span<const Wave> waves, DVec3 point,
                 double radius, GameTime time) {
    const DVec3 over = glm::normalize(point) * radius;
    // The water at rest that the waves carry over the point: found by stepping back along how
    // far they carry it, which shrinks while the crests don't fold.
    DVec3 rest = over;
    Surface surface = surface_at(trains, waves, rest, time);
    constexpr int steps = 12;
    for (int step = 0; step < steps; ++step) {
        const DVec3 up = glm::normalize(rest);
        const DVec3 carried = surface.moved - (up * glm::dot(up, surface.moved));
        const DVec3 missed = over - (rest + carried);
        const DVec3 off = missed - (up * glm::dot(up, missed));
        if (glm::dot(off, off) < 1e-12) {
            break;
        }
        rest = glm::normalize(rest + off) * radius;
        surface = surface_at(trains, waves, rest, time);
    }
    return glm::length(rest + surface.moved) - radius;
}

Strongest strongest(std::span<const Train> trains, std::span<const Wave> waves, std::size_t count) {
    std::array<std::vector<Wave>, bands> banded;
    for (const Wave& wave : waves) {
        banded.at(static_cast<std::size_t>(trains[wave.train].band)).push_back(wave);
    }
    for (std::vector<Wave>& band : banded) {
        std::ranges::sort(band,
                          [](const Wave& a, const Wave& b) { return a.amplitude > b.amplitude; });
    }
    Strongest kept;
    std::array<std::size_t, bands> taken{};
    for (bool any = true; any && kept.waves.size() < count;) {
        any = false;
        for (std::size_t band = 0; band < banded.size() && kept.waves.size() < count; ++band) {
            if (taken.at(band) < banded.at(band).size()) {
                kept.waves.push_back(banded.at(band)[taken.at(band)++]);
                any = true;
            }
        }
    }
    for (std::size_t band = 0; band < banded.size(); ++band) {
        for (std::size_t i = taken.at(band); i < banded.at(band).size(); ++i) {
            const Wave& wave = banded.at(band)[i];
            const double slope = trains[wave.train].wavenumber * wave.amplitude;
            kept.slope_left += slope * slope / 2.0;
        }
    }
    std::ranges::sort(kept.waves, [&](const Wave& a, const Wave& b) {
        return trains[a.train].wavenumber < trains[b.train].wavenumber;
    });
    return kept;
}

} // namespace hog::waves
