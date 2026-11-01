#include "../src/terms.hpp"

#include <hog/globe/cell.hpp>
#include <hog/globe/generator.hpp>
#include <hog/globe/planet.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdint>
#include <string_view>

using hog::DVec3;
using hog::globe::Planet;

namespace {

Planet named(std::string_view name) {
    return hog::globe::preset(name).value_or(Planet{});
}

// The share of the ground standing more than a metre off by craters, sampled with `lod`.
double cratered(std::string_view name, double lod) {
    const Planet planet = named(name);
    const hog::globe::Derived derived = hog::globe::derive(planet);
    const hog::globe::Generator::Shape shape;
    const hog::globe::terms::Context context{
        .planet = &planet, .derived = &derived, .shape = &shape};
    constexpr int count = 2'000;
    int marked = 0;
    for (std::uint64_t k = 0; k < count; ++k) {
        const DVec3 direction =
            hog::globe::center(hog::globe::cell_at_index(9, (k * 2'654'435'761ULL) % 1'572'864));
        marked += std::abs(hog::globe::terms::craters(context, direction, lod).value) > 1.0 ? 1 : 0;
    }
    return static_cast<double>(marked) / count;
}

} // namespace

TEST_CASE("simple craters are bowls and complex ones have floors and peaks", "[globe][craters]") {
    using hog::globe::terms::crater_profile;
    constexpr double transition = 15'000.0;
    // A bowl a fifth as deep as wide from rim to floor, its rim at one radius.
    const double diameter = 1'000.0;
    const double floor = crater_profile(0.0, diameter, transition, 0.0).height;
    const double rim = crater_profile(1.0, diameter, transition, 0.0).height;
    CHECK(std::abs((rim - floor) - (0.2 * diameter)) < 1e-9);
    CHECK(std::abs(rim - (0.036 * diameter)) < 1e-9);
    CHECK(crater_profile(0.5, diameter, transition, 0.0).height < rim);
    CHECK(crater_profile(1.5, diameter, transition, 0.0).height < rim);
    CHECK(crater_profile(1.5, diameter, transition, 0.0).height > 0.0);
    CHECK(crater_profile(2.5, diameter, transition, 0.0).height == 0.0);
    // Slopes match the heights, sharp-rimmed or rounded.
    for (const double size : {1'000.0, 60'000.0}) {
        for (const double rounding : {0.0, 0.2}) {
            for (int step = 0; step < 34; ++step) {
                const double x = 0.02 + (step * 0.0731);
                const double h = 1e-6;
                const double slope = (crater_profile(x + h, size, transition, rounding).height -
                                      crater_profile(x - h, size, transition, rounding).height) /
                                     (2.0 * h);
                CHECK(std::abs(slope - crater_profile(x, size, transition, rounding).slope) <
                      1e-3 * size);
            }
        }
    }
    // A complex crater is shallower for its size, with a flat floor and a peak in the middle.
    const double big = 60'000.0;
    const double big_floor = crater_profile(0.3, big, transition, 0.0).height;
    CHECK(crater_profile(1.0, big, transition, 0.0).height - big_floor < 0.1 * big);
    CHECK(crater_profile(0.25, big, transition, 0.0).height == big_floor);
    CHECK(crater_profile(0.0, big, transition, 0.0).height > big_floor + 1'000.0);
}

TEST_CASE("a worn crater's crest is rounded, its slope turning smoothly", "[globe][craters]") {
    using hog::globe::terms::crater_profile;
    constexpr double transition = 15'000.0;
    constexpr double diameter = 1'000.0;
    // Sharp, the slope jumps at the rim; rounded, it turns over the rounding's width.
    const double sharp_jump = crater_profile(1.0 - 1e-6, diameter, transition, 0.0).slope -
                              crater_profile(1.0 + 1e-6, diameter, transition, 0.0).slope;
    CHECK(sharp_jump > 100.0);
    double last = crater_profile(0.7, diameter, transition, 0.2).slope;
    for (int step = 1; step <= 60; ++step) {
        const double x = 0.7 + (0.01 * step);
        const double slope = crater_profile(x, diameter, transition, 0.2).slope;
        CHECK(std::abs(slope - last) < 0.1 * sharp_jump);
        last = slope;
    }
    // The crest still stands at the rim's height, and no wider than half a radius is rounded.
    CHECK(std::abs(crater_profile(1.0, diameter, transition, 0.2).height - (0.036 * diameter)) <
          1e-9);
    CHECK(crater_profile(0.49, diameter, transition, 3.0).height ==
          crater_profile(0.49, diameter, transition, 0.5).height);
}

TEST_CASE("old airless surfaces keep the most craters", "[globe][craters]") {
    // Neukum's chronology: a 4 Gyr surface has some 0.06 craters over a kilometre per km².
    CHECK(std::abs(hog::globe::terms::craters_over(named("moon"), 1'000.0) - 0.058) < 0.005);
    CHECK(hog::globe::terms::craters_over(named("earth"), 1'000.0) < 1e-4);
    // Venus's air stops everything that would leave a crater under about 3 km.
    CHECK(hog::globe::terms::craters_over(named("venus"), 100.0) ==
          hog::globe::terms::craters_over(named("venus"), 3'000.0));
    const double moon = cratered("moon", 4.0);
    const double mars = cratered("mars", 4.0);
    const double earth = cratered("earth", 4.0);
    UNSCOPED_INFO("cratered: moon " << moon << ", mars " << mars << ", earth " << earth);
    CHECK(moon > 0.5);
    CHECK(mars > 0.1);
    CHECK(mars < moon);
    CHECK(earth < 0.02);
}
