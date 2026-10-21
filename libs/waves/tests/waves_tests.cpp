#include <hog/waves/waves.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <random>
#include <vector>

using hog::DVec3;
using hog::GameDuration;
using hog::GameTime;
using hog::waves::SeaState;
using hog::waves::Train;
using hog::waves::Wave;

namespace {

constexpr std::uint64_t seed = 7;
constexpr double radius = 115'000.0;
constexpr double g = hog::waves::gravity;
constexpr DVec3 up{0.0, 0.0, 1.0};
constexpr DVec3 east{1.0, 0.0, 0.0};
constexpr DVec3 north{0.0, 1.0, 0.0};

// Late in 2026, so phases are taken far from the epoch, as they will be.
constexpr GameTime noon{std::chrono::seconds{1'792'000'000}};

GameTime later(double seconds) {
    return noon + std::chrono::duration_cast<GameDuration>(std::chrono::duration<double>{seconds});
}

DVec3 random_up(std::mt19937_64& random) {
    std::normal_distribution<double> normal;
    return glm::normalize(DVec3{normal(random), normal(random), normal(random)});
}

// A wind sea's significant height, as Pierson and Moskowitz found it.
double grown_height(double wind) {
    return 0.21 * (wind * 1.026) * (wind * 1.026) / g;
}

// The variance of the surface where the waves run, as their amplitudes and showing have it.
double variance_of(const std::vector<Train>& trains, const std::vector<Wave>& waves, DVec3 at) {
    double variance = 0.0;
    for (const Wave& wave : waves) {
        const double shown = hog::waves::showing(glm::dot(at, trains[wave.train].heading));
        variance += wave.amplitude * wave.amplitude * shown / 2.0;
    }
    return variance;
}

} // namespace

TEST_CASE("the planet's wave trains are fixed by its seed", "[waves]") {
    const std::vector<Train> trains = hog::waves::planet_trains(seed);
    const std::vector<Train> again = hog::waves::planet_trains(seed);
    const std::vector<Train> other = hog::waves::planet_trains(seed + 1);
    REQUIRE(trains.size() ==
            static_cast<std::size_t>(hog::waves::bands * hog::waves::trains_per_band));
    for (std::size_t i = 0; i < trains.size(); ++i) {
        CHECK(trains[i].heading == again[i].heading);
        CHECK(trains[i].frequency == again[i].frequency);
        CHECK(trains[i].phase == again[i].phase);
    }
    CHECK(trains.front().heading != other.front().heading);
    CHECK(trains.front().frequency != other.front().frequency);
}

TEST_CASE("each wave train runs at deep water's speed within its band", "[waves]") {
    for (const Train& train : hog::waves::planet_trains(seed)) {
        CHECK(std::abs(glm::length(train.heading) - 1.0) < 1e-12);
        CHECK(train.frequency >= hog::waves::band_lowest(train.band));
        CHECK(train.frequency <
              hog::waves::band_lowest(train.band) + hog::waves::band_width(train.band));
        CHECK(std::abs((train.frequency * train.frequency) - (g * train.wavenumber)) < 1e-9);
        CHECK(train.phase >= 0.0);
        CHECK(train.phase < 2.0 * std::numbers::pi);
    }
    CHECK(hog::waves::band_lowest(0) == hog::waves::lowest_frequency);
    CHECK(std::abs(hog::waves::band_lowest(hog::waves::bands) - hog::waves::highest_frequency) <
          1e-9);
}

TEST_CASE("every band has as many trains showing wherever on the planet", "[waves]") {
    const std::vector<Train> trains = hog::waves::planet_trains(seed);
    // Evenly spread, the trains' showing sums over a band to its share of the sphere.
    const double expected =
        hog::waves::trains_per_band * (hog::waves::fully_showing + hog::waves::last_showing) / 2.0;
    std::mt19937_64 random{3};
    for (int place = 0; place < 40; ++place) {
        const DVec3 at = random_up(random);
        for (int band = 0; band < hog::waves::bands; ++band) {
            double sum = 0.0;
            for (const Train& train : trains) {
                if (train.band == band) {
                    sum += hog::waves::showing(glm::dot(at, train.heading));
                }
            }
            CAPTURE(place, band, sum, expected);
            CHECK(std::abs(sum - expected) < expected * 0.3);
        }
    }
}

TEST_CASE("a train shows fully along the ground and not at all steeply off it", "[waves]") {
    CHECK(hog::waves::showing(0.0) == 1.0);
    CHECK(hog::waves::showing(-hog::waves::fully_showing) == 1.0);
    CHECK(hog::waves::showing(hog::waves::last_showing) == 0.0);
    CHECK(hog::waves::showing(-0.9) == 0.0);
    const double middle = (hog::waves::fully_showing + hog::waves::last_showing) / 2.0;
    CHECK(std::abs(hog::waves::showing(middle) - 0.5) < 1e-12);
}

TEST_CASE("the wind raises a sea as high as Pierson and Moskowitz found", "[waves]") {
    for (const double wind : {5.0, 10.0, 20.0}) {
        const SeaState sea{.wind = east * wind};
        double variance = 0.0;
        constexpr int steps = 200'000;
        const double step = 40.0 / steps;
        for (int i = 1; i <= steps; ++i) {
            variance += hog::waves::spectrum(sea, step * i) * step;
        }
        CAPTURE(wind);
        CHECK(std::abs((4.0 * std::sqrt(variance)) - grown_height(wind)) <
              grown_height(wind) * 0.01);
    }
    CHECK(hog::waves::spectrum({}, 1.0) == 0.0);
    // The peak lies where the longest waves run as fast as the wind.
    const SeaState sea{.wind = north * 10.0};
    const double peak = 0.877 * g / (10.0 * 1.026);
    CHECK(hog::waves::spectrum(sea, peak) > hog::waves::spectrum(sea, peak * 0.9));
    CHECK(hog::waves::spectrum(sea, peak) > hog::waves::spectrum(sea, peak * 1.1));
}

TEST_CASE("waves spread about the wind, none against it", "[waves]") {
    double total = 0.0;
    constexpr int steps = 10'000;
    for (int i = 0; i < steps; ++i) {
        const double across = (-std::numbers::pi) + ((2.0 * std::numbers::pi) * (i + 0.5) / steps);
        total += hog::waves::spreading(across) * (2.0 * std::numbers::pi / steps);
    }
    CHECK(std::abs(total - 1.0) < 1e-6);
    CHECK(hog::waves::spreading(0.0) > hog::waves::spreading(0.5));
    CHECK(hog::waves::spreading(std::numbers::pi) == 0.0);
}

TEST_CASE("the trains showing anywhere carry the sea the wind would raise there", "[waves]") {
    const std::vector<Train> trains = hog::waves::planet_trains(seed);
    std::mt19937_64 random{5};
    for (const double wind : {6.0, 12.0}) {
        double sum = 0.0;
        constexpr int places = 30;
        for (int place = 0; place < places; ++place) {
            const DVec3 at = random_up(random);
            const DVec3 along = glm::normalize(glm::cross(at, random_up(random)));
            const SeaState sea{.wind = along * wind};
            const std::vector<Wave> waves = hog::waves::waves_at(trains, at, sea, 0.0);
            sum += 4.0 * std::sqrt(variance_of(trains, waves, at));
        }
        CAPTURE(wind);
        CHECK(std::abs((sum / places) - grown_height(wind)) < grown_height(wind) * 0.1);
    }
    CHECK(hog::waves::waves_at(trains, up, {}).empty());
}

TEST_CASE("a calm sea lies flat", "[waves]") {
    const std::vector<Train> trains = hog::waves::planet_trains(seed);
    const hog::waves::Surface surface = hog::waves::surface_at(trains, {}, up * radius, noon);
    CHECK(surface.moved == DVec3{0.0});
    CHECK(glm::length(surface.normal - up) < 1e-12);
    CHECK(hog::waves::height_at(trains, {}, up * (radius + 3.0), radius, noon) == 0.0);
}

TEST_CASE("a wave's crests run along its heading at its speed", "[waves]") {
    const std::vector<Train> trains = hog::waves::planet_trains(seed);
    // The train running most nearly east over the top of the planet.
    std::uint32_t chosen = 0;
    double best = -1.0;
    for (std::uint32_t i = 0; i < trains.size(); ++i) {
        if (glm::dot(trains[i].heading, east) > best) {
            best = glm::dot(trains[i].heading, east);
            chosen = i;
        }
    }
    REQUIRE(best > 0.99);
    const Train& train = trains[chosen];
    const std::vector<Wave> waves{{.train = chosen, .amplitude = 0.5}};
    const double speed = train.frequency / train.wavenumber;
    const DVec3 heading = glm::normalize(train.heading - (up * glm::dot(up, train.heading)));
    constexpr double seconds = 3.0;
    const DVec3 there = glm::normalize((up * radius) + (heading * (speed * seconds))) * radius;
    for (const double x : {0.0, 1.3, 2.9}) {
        const DVec3 here = glm::normalize((up * radius) + (heading * x)) * radius;
        const DVec3 ahead = glm::normalize(there + (heading * x)) * radius;
        const double now = hog::waves::height_at(trains, waves, here, radius, noon);
        const double then = hog::waves::height_at(trains, waves, ahead, radius, later(seconds));
        CAPTURE(x);
        CHECK(std::abs(now - then) < 0.005);
    }
}

TEST_CASE("the water's surface faces as its neighbours lie", "[waves]") {
    const std::vector<Train> trains = hog::waves::planet_trains(seed);
    const SeaState sea{.wind = glm::normalize(east + north) * 9.0};
    const std::vector<Wave> waves = hog::waves::waves_at(trains, up, sea);
    REQUIRE(waves.size() > 40);
    std::mt19937_64 random{9};
    std::uniform_real_distribution<double> spot{-50.0, 50.0};
    constexpr double step = 0.002;
    for (int sample = 0; sample < 20; ++sample) {
        const DVec3 rest =
            glm::normalize((up * radius) + (east * spot(random)) + (north * spot(random))) * radius;
        const DVec3 frame_east = glm::normalize(glm::cross(north, rest));
        const DVec3 frame_north = glm::cross(glm::normalize(rest), frame_east);
        const auto moved = [&](DVec3 at) {
            const DVec3 on = glm::normalize(at) * radius;
            return on + hog::waves::surface_at(trains, waves, on, noon).moved;
        };
        const DVec3 one = moved(rest + (frame_east * step)) - moved(rest - (frame_east * step));
        const DVec3 two = moved(rest + (frame_north * step)) - moved(rest - (frame_north * step));
        const DVec3 found = glm::normalize(glm::cross(one, two));
        const DVec3 normal = hog::waves::surface_at(trains, waves, rest, noon).normal;
        CAPTURE(sample);
        CHECK(glm::dot(found, normal) > 0.99999);
    }
}

TEST_CASE("the height over a point is where the waves carry the water over it", "[waves]") {
    const std::vector<Train> trains = hog::waves::planet_trains(seed);
    const SeaState sea{.wind = north * 11.0};
    const std::vector<Wave> waves = hog::waves::waves_at(trains, up, sea);
    std::mt19937_64 random{11};
    std::uniform_real_distribution<double> spot{-200.0, 200.0};
    for (int sample = 0; sample < 50; ++sample) {
        const DVec3 rest =
            glm::normalize((up * radius) + (east * spot(random)) + (north * spot(random))) * radius;
        const DVec3 water = rest + hog::waves::surface_at(trains, waves, rest, noon).moved;
        const double height = hog::waves::height_at(trains, waves, water, radius, noon);
        CAPTURE(sample);
        CHECK(std::abs(height - (glm::length(water) - radius)) < 0.001);
    }
}

TEST_CASE("the sea's surface rises and falls as high as the wind raised it", "[waves]") {
    const std::vector<Train> trains = hog::waves::planet_trains(seed);
    const SeaState sea{.wind = east * 8.0};
    const std::vector<Wave> waves = hog::waves::waves_at(trains, up, sea);
    std::mt19937_64 random{13};
    std::uniform_real_distribution<double> spot{-2000.0, 2000.0};
    std::uniform_real_distribution<double> when{0.0, 600.0};
    double sum = 0.0;
    double squares = 0.0;
    constexpr int samples = 4000;
    for (int sample = 0; sample < samples; ++sample) {
        const DVec3 rest =
            glm::normalize((up * radius) + (east * spot(random)) + (north * spot(random))) * radius;
        const double height =
            glm::dot(glm::normalize(rest),
                     hog::waves::surface_at(trains, waves, rest, later(when(random))).moved);
        sum += height;
        squares += height * height;
    }
    const double mean = sum / samples;
    const double significant = 4.0 * std::sqrt((squares / samples) - (mean * mean));
    CHECK(std::abs(mean) < 0.05);
    CHECK(std::abs(significant - grown_height(8.0)) < grown_height(8.0) * 0.15);
}

TEST_CASE("the strongest waves are taken a band at a time, longest first", "[waves]") {
    const std::vector<Train> trains = hog::waves::planet_trains(seed);
    const SeaState sea{.wind = east * 10.0};
    const std::vector<Wave> waves = hog::waves::waves_at(trains, up, sea);
    REQUIRE(waves.size() > 48);
    const hog::waves::Strongest kept = hog::waves::strongest(trains, waves, 48);
    REQUIRE(kept.waves.size() == 48);
    CHECK(std::ranges::is_sorted(kept.waves, {},
                                 [&](const Wave& wave) { return trains[wave.train].wavenumber; }));
    // No band gives two before every band that can has given one.
    std::vector<int> per_band(hog::waves::bands, 0);
    std::vector<int> available(hog::waves::bands, 0);
    for (const Wave& wave : waves) {
        ++available.at(static_cast<std::size_t>(trains[wave.train].band));
    }
    for (const Wave& wave : kept.waves) {
        ++per_band.at(static_cast<std::size_t>(trains[wave.train].band));
    }
    const int most = std::ranges::max(per_band);
    for (int band = 0; band < hog::waves::bands; ++band) {
        const auto b = static_cast<std::size_t>(band);
        CHECK(per_band.at(b) >= std::min(available.at(b), most - 1));
    }
    // What's left out is all that's counted as slope left.
    double left = 0.0;
    double all = 0.0;
    for (const Wave& wave : waves) {
        const double slope = trains[wave.train].wavenumber * wave.amplitude;
        all += slope * slope / 2.0;
    }
    for (const Wave& wave : kept.waves) {
        const double slope = trains[wave.train].wavenumber * wave.amplitude;
        left += slope * slope / 2.0;
    }
    CHECK(std::abs(kept.slope_left - (all - left)) < 1e-12);
    CHECK(hog::waves::strongest(trains, waves, waves.size() + 5).waves.size() == waves.size());
    CHECK(hog::waves::strongest(trains, waves, waves.size() + 5).slope_left == 0.0);
}

TEST_CASE("even a gale's sea doesn't fold its crests over", "[waves]") {
    const std::vector<Train> trains = hog::waves::planet_trains(seed);
    const std::vector<Wave> waves = hog::waves::waves_at(trains, up, {.wind = east * 20.0});
    std::mt19937_64 random{17};
    std::uniform_real_distribution<double> spot{-500.0, 500.0};
    constexpr double step = 0.01;
    const auto moved = [&](DVec3 at) {
        const DVec3 on = glm::normalize(at) * radius;
        return on + hog::waves::surface_at(trains, waves, on, noon).moved;
    };
    for (int sample = 0; sample < 1000; ++sample) {
        const DVec3 rest = (up * radius) + (east * spot(random)) + (north * spot(random));
        const DVec3 one = moved(rest + (east * step)) - moved(rest - (east * step));
        const DVec3 two = moved(rest + (north * step)) - moved(rest - (north * step));
        // What a patch of water at rest is squeezed to along the ground.
        const double area = ((one.x * two.y) - (one.y * two.x)) / (4.0 * step * step);
        CAPTURE(sample);
        CHECK(area > 0.2);
    }
}
