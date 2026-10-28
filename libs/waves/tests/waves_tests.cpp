#include <hog/waves/shoaling.hpp>
#include <hog/waves/waves.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <random>
#include <vector>

using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;
using hog::DVec3;
using hog::GameDuration;
using hog::GameTime;
using hog::waves::SeaState;
using hog::waves::System;
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

SeaState windy(DVec3 wind) {
    return {.systems = {hog::waves::grown_sea(wind)}, .current = DVec3{0.0}, .wind = wind};
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
        const System sea = hog::waves::grown_sea(east * wind);
        CAPTURE(wind);
        CHECK(std::abs(sea.height - grown_height(wind)) < grown_height(wind) * 0.01);
        // The peak lies where the longest waves run a little faster than the wind.
        CHECK(std::abs(sea.peak - (0.877 * g / (wind * 1.026))) < sea.peak * 0.001);
        CHECK(glm::length(sea.heading - east) < 1e-12);
        CHECK(sea.peakedness == 1.0);
    }
    CHECK(hog::waves::grown_sea(DVec3{0.0}).height == 0.0);
}

TEST_CASE("a system's bands carry its height, most of it about its peak", "[waves]") {
    for (const double peakedness : {1.0, hog::waves::wind_sea_peakedness}) {
        const System sea{.height = 3.0, .peak = 0.9, .peakedness = peakedness};
        const auto banded = hog::waves::band_variances(sea);
        double sum = 0.0;
        for (const double variance : banded) {
            CHECK(variance >= 0.0);
            sum += variance;
        }
        CAPTURE(peakedness);
        CHECK(std::abs(sum - (3.0 * 3.0 / 16.0)) < 1e-12);
        std::size_t peak_band = 0;
        while (hog::waves::band_lowest(static_cast<int>(peak_band) + 1) <= sea.peak) {
            ++peak_band;
        }
        CHECK(std::ranges::max_element(banded) - banded.begin() ==
              static_cast<std::ptrdiff_t>(peak_band));
    }
    // A swell, run far from its wind, gathers more of its variance about its peak.
    const System wide{.height = 2.0, .peak = 0.6};
    const System swell{.height = 2.0, .peak = 0.6, .peakedness = hog::waves::swell_peakedness};
    CHECK(std::ranges::max(hog::waves::band_variances(swell)) >
          1.3 * std::ranges::max(hog::waves::band_variances(wide)));
    CHECK(std::ranges::max(hog::waves::band_variances(System{})) == 0.0);
}

TEST_CASE("waves spread about their system's heading, none against it", "[waves]") {
    for (const double spread : {2.0, hog::waves::swell_spread}) {
        const System system{.spread = spread};
        double total = 0.0;
        constexpr int steps = 10'000;
        for (int i = 0; i < steps; ++i) {
            const double across =
                (-std::numbers::pi) + ((2.0 * std::numbers::pi) * (i + 0.5) / steps);
            total +=
                hog::waves::spreading(system, std::cos(across)) * (2.0 * std::numbers::pi / steps);
        }
        CAPTURE(spread);
        CHECK(std::abs(total - 1.0) < 1e-6);
        CHECK(hog::waves::spreading(system, 1.0) > hog::waves::spreading(system, 0.9));
        CHECK(hog::waves::spreading(system, -0.5) == 0.0);
    }
    // As the wind sea always spread.
    CHECK(std::abs(hog::waves::spreading(System{}, 1.0) - (2.0 / std::numbers::pi)) < 1e-12);
}

TEST_CASE("a current steepens the waves it runs against and stretches those it runs with",
          "[waves]") {
    constexpr double speed = 8.0;
    CHECK(hog::waves::steepened(speed, 0.0) == 1.0);
    CHECK(hog::waves::steepened(speed, 1.0) < 1.0);
    CHECK(hog::waves::steepened(speed, -0.5) > 1.0);
    // Against a current a quarter their speed they can't make headway, and none get through.
    CHECK(hog::waves::steepened(speed, -speed / 4.0) == 0.0);
    CHECK(hog::waves::steepened(speed, -speed) == 0.0);
    double last = hog::waves::steepened(speed, -0.24 * speed);
    for (int step = -240; step < 1000; ++step) {
        const double current = speed * step / 1000.0;
        const double factor = hog::waves::steepened(speed, current);
        CAPTURE(current);
        CHECK(factor <= 2.0 + 1e-6);
        CHECK(std::abs(factor - last) < 0.05);
        last = factor;
    }
    CHECK(hog::waves::steepened(0.0, 1.0) == 1.0);
}

TEST_CASE("the trains showing anywhere carry all the sea's systems", "[waves]") {
    const std::vector<Train> trains = hog::waves::planet_trains(seed);
    std::mt19937_64 random{5};
    for (int place = 0; place < 30; ++place) {
        const DVec3 at = random_up(random);
        const DVec3 along = glm::normalize(glm::cross(at, random_up(random)));
        const DVec3 aside = glm::cross(at, along);
        const SeaState sea{.systems = {hog::waves::grown_sea(along * 9.0),
                                       {.heading = aside,
                                        .height = 1.5,
                                        .peak = 0.5,
                                        .peakedness = hog::waves::swell_peakedness,
                                        .spread = hog::waves::swell_spread}},
                           .current = DVec3{0.0},
                           .wind = DVec3{0.0}};
        const double expected = std::hypot(grown_height(9.0), 1.5);
        const std::vector<Wave> waves = hog::waves::waves_at(trains, at, sea, 0.0);
        CAPTURE(place);
        CHECK(std::abs((4.0 * std::sqrt(variance_of(trains, waves, at))) - expected) <
              expected * 0.01);
    }
    CHECK(hog::waves::waves_at(trains, up, {}).empty());
}

TEST_CASE("waves run mostly along their system's heading", "[waves]") {
    const std::vector<Train> trains = hog::waves::planet_trains(seed);
    const SeaState sea{.systems = {{.heading = north,
                                    .height = 2.0,
                                    .peak = 0.6,
                                    .peakedness = hog::waves::swell_peakedness,
                                    .spread = hog::waves::swell_spread}},
                       .current = DVec3{0.0},
                       .wind = DVec3{0.0}};
    double ahead = 0.0;
    double all = 0.0;
    for (const Wave& wave : hog::waves::waves_at(trains, up, sea, 0.0)) {
        const DVec3 heading = trains[wave.train].heading;
        const double variance =
            wave.amplitude * wave.amplitude * hog::waves::showing(heading.z) / 2.0;
        all += variance;
        if (glm::dot(glm::normalize(heading - (up * heading.z)), north) > std::cos(0.6)) {
            ahead += variance;
        }
    }
    CHECK(ahead > 0.9 * all);
}

TEST_CASE("the sea's current steepens the waves running against it", "[waves]") {
    const std::vector<Train> trains = hog::waves::planet_trains(seed);
    SeaState sea = windy(east * 10.0);
    const std::vector<Wave> still = hog::waves::waves_at(trains, up, sea, 0.0);
    sea.current = east * -0.4;
    const std::vector<Wave> against = hog::waves::waves_at(trains, up, sea, 0.0);
    REQUIRE(!still.empty());
    std::size_t matched = 0;
    for (const Wave& wave : still) {
        const Train& train = trains[wave.train];
        const DVec3 along = glm::normalize(train.heading - (up * train.heading.z));
        const double factor =
            hog::waves::steepened(train.frequency / train.wavenumber, -0.4 * along.x);
        const auto found = std::ranges::find(against, wave.train, &Wave::train);
        if (factor == 0.0) {
            CHECK(found == against.end());
            continue;
        }
        REQUIRE(found != against.end());
        CHECK(std::abs(found->amplitude - (wave.amplitude * factor)) < 1e-12);
        ++matched;
    }
    CHECK(matched > 10);
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
    const SeaState sea = windy(glm::normalize(east + north) * 9.0);
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
    const SeaState sea = windy(north * 11.0);
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
    const SeaState sea = windy(east * 8.0);
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
    const SeaState sea = windy(east * 10.0);
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
    const std::vector<Wave> waves = hog::waves::waves_at(trains, up, windy(east * 20.0));
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

namespace {

// The floor `out` metres from the waterline of a sandy shore whose sea lies east, as the
// planet's sea floor has it.
hog::waves::Floor shore_floor(double out) {
    const double shoreface = hog::waves::dean_scale * std::cbrt(out * out);
    if (shoreface > out / hog::waves::shelf_run) {
        return {.depth = shoreface,
                .deepening = east * (2.0 / 3.0 * hog::waves::dean_scale / std::cbrt(out)),
                .out = out,
                .seaward = east};
    }
    return {.depth = out / hog::waves::shelf_run,
            .deepening = east / hog::waves::shelf_run,
            .out = out,
            .seaward = east};
}

// The train running most nearly along `heading` over the top of the planet.
std::uint32_t train_along(const std::vector<Train>& trains, DVec3 heading, int band) {
    std::uint32_t chosen = 0;
    double best = -1.0;
    for (std::uint32_t i = 0; i < trains.size(); ++i) {
        if (trains[i].band == band && glm::dot(trains[i].heading, heading) > best) {
            best = glm::dot(trains[i].heading, heading);
            chosen = i;
        }
    }
    return chosen;
}

} // namespace

TEST_CASE("over deep water the floor changes nothing", "[waves][shoaling]") {
    const std::vector<Train> trains = hog::waves::planet_trains(seed);
    const std::vector<Wave> waves = hog::waves::waves_at(trains, up, windy(north * 9.0));
    const DVec3 rest = glm::normalize((up * radius) + (east * 31.0)) * radius;
    const hog::waves::Surface open = hog::waves::surface_at(trains, waves, rest, noon);
    const hog::waves::Surface over = hog::waves::surface_at(
        trains, waves, rest, noon, {.depth = 4'000.0, .deepening = east * 0.1});
    CHECK(glm::distance(open.moved, over.moved) < 1e-9);
    CHECK(glm::distance(open.normal, over.normal) < 1e-9);
}

TEST_CASE("in the shallows the water's surface still faces as its neighbours lie",
          "[waves][shoaling]") {
    const std::vector<Train> trains = hog::waves::planet_trains(seed);
    // A swell running in at the shore, and a breeze along it.
    SeaState sea = windy(north * 6.0);
    sea.systems.push_back({.heading = glm::normalize(north - east),
                           .height = 1.2,
                           .peak = 0.6,
                           .peakedness = hog::waves::swell_peakedness,
                           .spread = hog::waves::swell_spread});
    const std::vector<Wave> waves = hog::waves::waves_at(trains, up, sea);
    constexpr double step = 0.002;
    for (const double out : {20.0, 150.0, 900.0, 3'000.0}) {
        for (const double along : {-7.0, 0.0, 13.0}) {
            const auto at = [&](double east_of) {
                return glm::normalize((up * radius) + (east * east_of) + (north * along)) * radius;
            };
            const auto moved = [&](double east_of, double north_of) {
                const DVec3 on = glm::normalize(at(east_of) + (north * north_of)) * radius;
                return on +
                       hog::waves::surface_at(trains, waves, on, noon, shore_floor(out + east_of))
                           .moved;
            };
            const DVec3 one = moved(step, 0.0) - moved(-step, 0.0);
            const DVec3 two = moved(0.0, step) - moved(0.0, -step);
            const DVec3 found = glm::normalize(glm::cross(one, two));
            const DVec3 normal =
                hog::waves::surface_at(trains, waves, at(0.0), noon, shore_floor(out)).normal;
            CAPTURE(out, along);
            CHECK(glm::dot(found, normal) > 0.9995);
        }
    }
}

TEST_CASE("waves running in shorten and turn towards the shore", "[waves][shoaling]") {
    const std::vector<Train> trains = hog::waves::planet_trains(seed);
    // A swell of about 9 s running in at 45 degrees, tiny so it doesn't break.
    const DVec3 slant = glm::normalize(north - east);
    const std::uint32_t chosen = train_along(trains, slant, 3);
    const Train& train = trains[chosen];
    REQUIRE(glm::dot(train.heading, slant) > 0.97);
    const std::vector<Wave> waves{{.train = chosen, .amplitude = 0.001}};
    constexpr double out = 60.0;
    const hog::waves::Floor floor = shore_floor(out);
    // The heights round a point, whose gradient runs along the wave's wavevector.
    constexpr double step = 0.01;
    const auto height = [&](double east_of, double north_of, GameTime time) {
        const DVec3 on =
            glm::normalize((up * radius) + (east * east_of) + (north * north_of)) * radius;
        return glm::dot(
            up, hog::waves::surface_at(trains, waves, on, time, shore_floor(out + east_of)).moved);
    };
    // When the slope's steepest here.
    double steepest = 0.0;
    hog::DVec2 gradient{0.0};
    for (int tick = 0; tick < 40; ++tick) {
        const GameTime time = later(tick * 0.25);
        const hog::DVec2 found{(height(step, 0.0, time) - height(-step, 0.0, time)) / (2.0 * step),
                               (height(0.0, step, time) - height(0.0, -step, time)) / (2.0 * step)};
        if (glm::length(found) > steepest) {
            steepest = glm::length(found);
            gradient = found;
        }
    }
    const double incoming =
        std::acos(std::abs(glm::dot(glm::normalize(gradient), hog::DVec2{-1.0, 0.0})));
    // Turned well round from 45 degrees off the shore's normal, as Snell's law would have it.
    const double deep = train.wavenumber;
    const double local = hog::waves::shallow_wavenumber(deep, floor.depth);
    const double snell = std::asin(std::sin(std::numbers::pi / 4.0) * deep / local);
    CHECK(incoming < (std::numbers::pi / 4.0) - 0.1);
    CHECK(std::abs(incoming - snell) < 0.15);
    // And shortened: the slope over the height is the local wavenumber.
    const double amplitude = 0.001 * hog::waves::shoaling(deep, floor.depth);
    CHECK(steepest / amplitude > deep * 1.5);
}

TEST_CASE("in the shallows waves break down to what the depth holds", "[waves][shoaling]") {
    const std::vector<Train> trains = hog::waves::planet_trains(seed);
    // A gale's sea running in at the shore, and one running along it: what of it comes in
    // still breaks down to the depth.
    const auto heading = GENERATE(glm::normalize(-east + (north * 0.3)), north);
    const SeaState sea = windy(heading * 12.0);
    const std::vector<Wave> waves = hog::waves::waves_at(trains, up, sea);
    std::mt19937_64 random{17};
    std::uniform_real_distribution<double> spot{-300.0, 300.0};
    std::uniform_real_distribution<double> when{0.0, 600.0};
    for (const double out : {10.0, 60.0}) {
        const hog::waves::Floor floor = shore_floor(out);
        double squares = 0.0;
        constexpr int samples = 3000;
        for (int sample = 0; sample < samples; ++sample) {
            const DVec3 rest = glm::normalize((up * radius) + (north * spot(random))) * radius;
            const double height = glm::dot(
                glm::normalize(rest),
                hog::waves::surface_at(trains, waves, rest, later(when(random)), floor).moved);
            squares += height * height;
        }
        // The root-mean-square height is the root of eight times the variance.
        const double rms = std::sqrt(8.0 * squares / samples);
        CAPTURE(heading, out, floor.depth, rms);
        CHECK(rms < hog::waves::breaking_depth * floor.depth * 1.1);
        CHECK(rms > hog::waves::breaking_depth * floor.depth * 0.9);
    }
}

TEST_CASE("in the shallows a swell's crests steepen but don't fold over", "[waves][shoaling]") {
    const std::vector<Train> trains = hog::waves::planet_trains(seed);
    // A long, high swell running in at the shore, and a breeze's sea across it.
    SeaState sea = windy(north * 7.0);
    sea.systems.push_back({.heading = glm::normalize(-east + (north * 0.2)),
                           .height = 1.6,
                           .peak = 0.11,
                           .peakedness = hog::waves::swell_peakedness,
                           .spread = hog::waves::swell_spread});
    const std::vector<Wave> waves = hog::waves::waves_at(trains, up, sea);
    std::mt19937_64 random{23};
    std::uniform_real_distribution<double> spot{-200.0, 200.0};
    std::uniform_real_distribution<double> when{0.0, 600.0};
    constexpr double step = 0.01;
    double least = 1.0;
    const double slope = GENERATE(0.0, 0.02, 0.05, 0.1, 0.2);
    const auto beach = [&](double out_by) {
        return slope > 0.0 ? hog::waves::Floor{.depth = out_by * slope,
                                               .deepening = east * slope,
                                               .out = out_by,
                                               .seaward = east}
                           : shore_floor(out_by);
    };
    for (const double out : {8.0, 25.0, 60.0, 150.0}) {
        for (int sample = 0; sample < 1500; ++sample) {
            const double along = spot(random);
            const GameTime time = later(when(random));
            const auto moved = [&](double east_of, double north_of) {
                const DVec3 on = glm::normalize((up * radius) + (east * east_of) +
                                                (north * (along + north_of))) *
                                 radius;
                return on +
                       hog::waves::surface_at(trains, waves, on, time, beach(out + east_of)).moved;
            };
            const DVec3 one = moved(step, 0.0) - moved(-step, 0.0);
            const DVec3 two = moved(0.0, step) - moved(0.0, -step);
            // What a patch of water at rest is squeezed to along the ground.
            const double area = ((glm::dot(one, east) * glm::dot(two, north)) -
                                 (glm::dot(one, north) * glm::dot(two, east))) /
                                (4.0 * step * step);
            least = std::min(least, area);
        }
    }
    CAPTURE(slope, least);
    CHECK(least > 0.1);
}

TEST_CASE("over a steep floor the crests keep as far apart as over the shore's profile",
          "[waves][shoaling]") {
    const std::vector<Train> trains = hog::waves::planet_trains(seed);
    // A swell of about 9 s running straight in, tiny so it doesn't break.
    const std::uint32_t chosen = train_along(trains, -east, 3);
    const std::vector<Wave> waves{{.train = chosen, .amplitude = 0.001}};
    constexpr double out = 40.0;
    // Over a floor falling as `steep` from the waterline, the slope of the heights round a point
    // over their height, steepest: the wavenumber there.
    const auto wavenumber = [&](double steep, bool known) {
        const auto floor = [&](double out_by) {
            hog::waves::Floor found{.depth = out_by * steep, .deepening = east * steep};
            if (known) {
                found.out = out_by;
                found.seaward = east;
            }
            return found;
        };
        constexpr double step = 0.01;
        const auto height = [&](double east_of, GameTime time) {
            const DVec3 on = glm::normalize((up * radius) + (east * east_of)) * radius;
            return glm::dot(
                up, hog::waves::surface_at(trains, waves, on, time, floor(out + east_of)).moved);
        };
        double steepest = 0.0;
        double highest = 0.0;
        for (int tick = 0; tick < 80; ++tick) {
            const GameTime time = later(tick * 0.125);
            steepest = std::max(steepest,
                                std::abs(height(step, time) - height(-step, time)) / (2.0 * step));
            highest = std::max(highest, std::abs(height(0.0, time)));
        }
        return steepest / highest;
    };
    const double deep = trains[chosen].wavenumber;
    const double profiled = hog::waves::shallow_wavenumber(deep, hog::waves::profile_depth(out));
    // Read as deep as it is, a floor ten times steeper than the profile crowds the crests.
    CHECK(wavenumber(0.1, false) > profiled * 2.0);
    CHECK(wavenumber(0.1, true) == Catch::Approx(profiled).epsilon(0.1));
    CHECK(wavenumber(0.005, true) == Catch::Approx(profiled).epsilon(0.1));
}

TEST_CASE("the shore's profile deepens as its depth says, the shelf taking over unbroken",
          "[waves][shoaling]") {
    using hog::waves::profile_depth;
    using hog::waves::profile_slope;
    constexpr double turn = hog::waves::shelf_depth * hog::waves::shelf_run;
    CHECK(profile_depth(0.0) == 0.0);
    CHECK(profile_depth(-5.0) == 0.0);
    CHECK(profile_depth(turn) == Catch::Approx(hog::waves::shelf_depth));
    CHECK(profile_depth(turn - 1e-6) == Catch::Approx(profile_depth(turn + 1e-6)));
    CHECK(profile_depth(8'000.0) == Catch::Approx(80.0));
    for (const double out : {0.5, 3.0, 40.0, 900.0, turn - 50.0, turn + 50.0, 4'000.0}) {
        constexpr double step = 0.001;
        CAPTURE(out);
        CHECK(profile_slope(out) ==
              Catch::Approx((profile_depth(out + step) - profile_depth(out - step)) / (2.0 * step))
                  .epsilon(1e-4));
    }
    CHECK(std::isfinite(profile_slope(0.0)));
}

TEST_CASE("a wind over a short fetch raises lower, shorter waves", "[waves]") {
    using hog::waves::fetched;
    constexpr double speed = 10.0;
    const System grown = hog::waves::grown_sea({speed, 0.0, 0.0});
    // Pierson and Moskowitz's spectrum, as grown_sea has it.
    const auto spectrum = [&](double frequency) {
        const double ratio = grown.peak / frequency;
        return 5.0 * grown.height * grown.height / 16.0 / grown.peak * std::pow(ratio, 5.0) *
               std::exp(-1.25 * std::pow(ratio, 4.0));
    };
    for (const double fetch : {300.0, 3'000.0, 30'000.0}) {
        CAPTURE(fetch);
        double variance = 0.0;
        double peak = 0.0;
        double highest = 0.0;
        constexpr int steps = 20'000;
        const double step = std::log(1'000.0) / steps;
        for (int i = 0; i < steps; ++i) {
            const double frequency = 0.1 * std::exp((i + 0.5) * step);
            const double density =
                std::pow(fetched(frequency, fetch, speed), 2.0) * spectrum(frequency);
            variance += density * frequency * step;
            if (density > highest) {
                highest = density;
                peak = frequency;
            }
        }
        // As JONSWAP's laws have it.
        const double reach = g * fetch / (speed * speed);
        CHECK_THAT(
            variance,
            WithinRel(hog::waves::fetch_growth * reach * std::pow(speed, 4.0) / (g * g), 0.01));
        CHECK_THAT(peak, WithinRel(hog::waves::fetch_peak_scale *
                                       std::pow(reach, -hog::waves::fetch_peak_power) * g / speed,
                                   0.01));
        // Shorter waves grow first.
        CHECK(fetched(peak * 3.0, fetch, speed) > fetched(peak / 1.5, fetch, speed));
    }
    // Grown full, as grown_sea; and none without water to grow over.
    for (const double frequency : {0.5, 1.0, 4.0}) {
        CHECK_THAT(fetched(frequency, 1e6, speed), WithinAbs(1.0, 1e-12));
        CHECK(fetched(frequency, 0.0, speed) == 0.0);
    }
}
