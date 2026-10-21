#include <hog/core/hash.hpp>
#include <hog/waves/shoaling.hpp>
#include <hog/waves/waves.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <numbers>
#include <utility>

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

System grown_sea(DVec3 wind) {
    const double speed = glm::length(wind) * wind_19_5;
    if (speed <= 0.0) {
        return {};
    }
    // Pierson and Moskowitz's spectrum integrates to α U⁴ / 4βg², and peaks where its slope
    // turns.
    return {.heading = wind / glm::length(wind),
            .height = 4.0 * std::sqrt(pm_alpha / (4.0 * pm_beta)) * speed * speed / gravity,
            .peak = std::pow(0.8 * pm_beta, 0.25) * gravity / speed,
            .peakedness = 1.0,
            .spread = 2.0};
}

std::array<double, bands> band_variances(const System& system) {
    std::array<double, bands> variances{};
    if (system.height <= 0.0 || system.peak <= 0.0) {
        return variances;
    }
    const auto shape = [&](double frequency) {
        const double ratio = system.peak / frequency;
        const double fourth = ratio * ratio * ratio * ratio;
        const double width = frequency <= system.peak ? 0.07 : 0.09;
        const double off = (frequency - system.peak) / (width * system.peak);
        return fourth * ratio * std::exp(-1.25 * fourth) *
               std::pow(system.peakedness, std::exp(-off * off / 2.0));
    };
    // Sampled finely enough within each band that even a narrow swell's peak isn't missed.
    constexpr int samples = 8;
    double sum = 0.0;
    for (int band = 0; band < bands; ++band) {
        double variance = 0.0;
        double from = band_lowest(band);
        for (int sample = 1; sample <= samples; ++sample) {
            const double to =
                band_lowest(band) * std::pow(band_ratio, static_cast<double>(sample) / samples);
            variance += shape(std::sqrt(from * to)) * (to - from);
            from = to;
        }
        variances.at(static_cast<std::size_t>(band)) = variance;
        sum += variance;
    }
    const double scale = sum > 0.0 ? system.height * system.height / 16.0 / sum : 0.0;
    for (double& variance : variances) {
        variance *= scale;
    }
    return variances;
}

double spreading(const System& system, double along) {
    if (along <= 0.0) {
        return 0.0;
    }
    const double half = system.spread / 2.0;
    return std::tgamma(half + 1.0) / (std::sqrt(std::numbers::pi) * std::tgamma(half + 0.5)) *
           std::pow(along, system.spread);
}

double steepened(double speed, double current) {
    if (speed <= 0.0) {
        return 1.0;
    }
    // Longuet-Higgins and Stewart's, in deep water: with s the root of 1 + 4U/c, a wave's height
    // grows by 2 / (1 + s)√s, without bound as an opposing current nears a quarter of its speed.
    // Before then it has doubled, and breaks, and less of it gets through until none does.
    constexpr double most = 2.0;
    constexpr double breaking_root = 0.46557; // where (1 + s)√s is 1
    const double root_squared = 1.0 + (4.0 * current / speed);
    if (root_squared <= 0.0) {
        return 0.0;
    }
    const double root = std::sqrt(root_squared);
    if (root < breaking_root) {
        return most * root_squared / (breaking_root * breaking_root);
    }
    return 2.0 / ((1.0 + root) * std::sqrt(root));
}

std::vector<Wave> waves_at(std::span<const Train> trains, DVec3 up, const SeaState& sea,
                           double least) {
    struct Here {
        std::uint32_t train = 0;
        double shown = 0.0;
        DVec3 along{0.0};
    };
    std::vector<Here> here;
    for (std::size_t i = 0; i < trains.size(); ++i) {
        const double rising = glm::dot(up, trains[i].heading);
        const double shown = showing(rising);
        if (shown > 0.0) {
            here.push_back({.train = static_cast<std::uint32_t>(i),
                            .shown = shown,
                            .along = glm::normalize(trains[i].heading - (up * rising))});
        }
    }
    // Each system's variance in a band goes to its trains showing here by how near they run to
    // its heading, and how much they show: so all of it lands, however few run near.
    std::vector<double> variances(here.size());
    std::vector<double> spread(here.size());
    for (const System& system : sea.systems) {
        if (system.height <= 0.0) {
            continue;
        }
        const std::array<double, bands> banded = band_variances(system);
        std::array<double, bands> totals{};
        for (std::size_t i = 0; i < here.size(); ++i) {
            spread[i] = spreading(system, glm::dot(here[i].along, system.heading));
            totals.at(static_cast<std::size_t>(trains[here[i].train].band)) +=
                spread[i] * here[i].shown;
        }
        for (std::size_t i = 0; i < here.size(); ++i) {
            const auto band = static_cast<std::size_t>(trains[here[i].train].band);
            if (totals.at(band) > 0.0) {
                variances[i] += banded.at(band) * spread[i] / totals.at(band);
            }
        }
    }
    std::vector<Wave> waves;
    for (std::size_t i = 0; i < here.size(); ++i) {
        const Train& train = trains[here[i].train];
        const double amplitude =
            std::sqrt(2.0 * variances[i]) *
            steepened(train.frequency / train.wavenumber, glm::dot(sea.current, here[i].along));
        if (amplitude > 0.0 && amplitude >= least) {
            waves.push_back({.train = here[i].train, .amplitude = amplitude});
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
                   GameTime time, const Floor& floor) {
    const DVec3 up = glm::normalize(rest);
    const bool shallow = std::isfinite(floor.depth);
    const DVec3 deepening = floor.deepening - (up * glm::dot(up, floor.deepening));
    const double steepness = glm::length(deepening);
    const DVec3 offshore = steepness > 0.0 ? deepening / steepness : DVec3{0.0};
    // Each wave as it shows here, along the ground: how high and its wavenumber in deep water.
    const auto shown_at = [&](const Wave& wave) {
        const Train& train = trains[wave.train];
        const double rising = glm::dot(up, train.heading);
        const double shown = std::sqrt(showing(rising));
        const double flat = std::sqrt(std::max(1.0 - (rising * rising), 0.0));
        return std::pair{wave.amplitude * shown, train.wavenumber * flat};
    };
    // Those too high for the depth break, as many as it takes to bring them down to it.
    double kept = 1.0;
    if (shallow) {
        double squares = 0.0;
        for (const Wave& wave : waves) {
            const auto [amplitude, deep] = shown_at(wave);
            const double shoaled = amplitude * shoaling(deep, floor.depth);
            squares += shoaled * shoaled;
        }
        kept = depth_limited(2.0 * std::sqrt(squares), floor.depth);
    }
    Surface surface;
    // How the surface slopes, and how its water bunches along the ground, against the ground
    // beneath at rest.
    DVec3 slope{0.0};
    glm::dmat3 bunching{0.0};
    for (const Wave& wave : waves) {
        const Train& train = trains[wave.train];
        auto [amplitude, wavenumber] = shown_at(wave);
        if (amplitude <= 0.0) {
            continue;
        }
        const double rising = glm::dot(up, train.heading);
        DVec3 heading = glm::normalize(train.heading - (up * rising));
        double phase = phase_at(train, rest, time);
        double stretch = 1.0;
        if (shallow) {
            const double deep = wavenumber;
            // Waves running out to sea where it's shallow have come off the shore, and are lost.
            const double thinned = deep / shallow_wavenumber(deep, floor.depth);
            const double towards = std::clamp((0.3 - glm::dot(heading, offshore)) / 0.6, 0.0, 1.0);
            const double sheltered = towards * towards * (3.0 - (2.0 * towards));
            amplitude *= shoaling(deep, floor.depth) * kept * std::lerp(sheltered, 1.0, thinned);
            stretch = orbit_stretch(deep, floor.depth);
            phase += phase_lag(deep, floor.depth);
            // The lag's gradient turns and shortens it.
            const DVec3 turned =
                (heading * deep) + (deepening * phase_lag_slope(deep, floor.depth));
            wavenumber = glm::length(turned);
            if (wavenumber <= 0.0) {
                continue;
            }
            heading = turned / wavenumber;
        }
        const double c = std::cos(phase);
        const double s = std::sin(phase);
        const double swing = choppiness * amplitude * stretch;
        surface.moved += (up * (amplitude * c)) - (heading * (swing * s));
        slope += heading * (amplitude * wavenumber * s);
        bunching += glm::outerProduct(heading, heading) * (swing * wavenumber * c);
    }
    const auto [first, second] = across(up);
    const DVec3 one = first - (up * glm::dot(slope, first)) - (bunching * first);
    const DVec3 two = second - (up * glm::dot(slope, second)) - (bunching * second);
    surface.normal = glm::normalize(glm::cross(one, two));
    return surface;
}

double height_at(std::span<const Train> trains, std::span<const Wave> waves, DVec3 point,
                 double radius, GameTime time, const Floor& floor) {
    const DVec3 over = glm::normalize(point) * radius;
    // The water at rest that the waves carry over the point: found by stepping back along how
    // far they carry it, which shrinks while the crests don't fold.
    DVec3 rest = over;
    Surface surface = surface_at(trains, waves, rest, time, floor);
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
        surface = surface_at(trains, waves, rest, time, floor);
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
