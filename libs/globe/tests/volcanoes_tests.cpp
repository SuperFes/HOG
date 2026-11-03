#include "../src/terms.hpp"

#include <hog/globe/cell.hpp>
#include <hog/globe/generator.hpp>
#include <hog/globe/planet.hpp>
#include <hog/globe/vents.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>

using hog::DVec3;
using hog::globe::Planet;
using hog::globe::Volcano;

namespace {

Volcano first_volcano(const Planet& planet) {
    for (std::uint64_t index = 0;; ++index) {
        if (const auto volcano =
                hog::globe::volcano_in(planet, hog::globe::cell_at_index(6, index))) {
            return *volcano;
        }
    }
}

// The direction `metres` from the volcano along `bearing`.
DVec3 toward(const Planet& planet, const Volcano& volcano, double bearing, double metres) {
    const DVec3 axis = std::abs(volcano.centre.z) < 0.9 ? DVec3{0.0, 0.0, 1.0} : DVec3{1.0, 0.0, 0.0};
    const DVec3 east = glm::normalize(glm::cross(axis, volcano.centre));
    const DVec3 north = glm::cross(volcano.centre, east);
    return glm::normalize(volcano.centre + (((std::cos(bearing) * east) + (std::sin(bearing) * north)) *
                                            (metres / planet.radius)));
}

} // namespace

TEST_CASE("a volcano is a cone with a crater, its outline not round", "[globe][volcanoes]") {
    const Planet planet;
    const hog::globe::Derived derived = hog::globe::derive(planet);
    const hog::globe::Generator::Shape shape;
    const hog::globe::terms::Context context{
        .planet = &planet, .derived = &derived, .shape = &shape};
    const Volcano volcano = first_volcano(planet);
    const auto height = [&](DVec3 direction) {
        return hog::globe::terms::volcanoes(context, direction, 0.5);
    };
    // The summit lip stands over the crater floor, and the flank over the foot.
    const double floor = height(volcano.centre).value;
    const double lip = height(toward(planet, volcano, 0.3, 0.2 * volcano.radius)).value;
    CHECK(lip > floor);
    CHECK(lip > height(toward(planet, volcano, 0.3, 0.7 * volcano.radius)).value);
    CHECK(height(toward(planet, volcano, 0.3, 0.7 * volcano.radius)).value >
          height(toward(planet, volcano, 0.3, 0.97 * volcano.radius * 1.3)).value);
    // Nothing beyond its reach.
    CHECK(height(toward(planet, volcano, 1.0, 3.0 * volcano.radius)).value == 0.0);
    // The same distance out, different bearings stand at different heights.
    double least = 1e9;
    double most = -1e9;
    for (int k = 0; k < 12; ++k) {
        const double h = height(toward(planet, volcano, k * 0.5236, 0.6 * volcano.radius)).value;
        least = std::min(least, h);
        most = std::max(most, h);
    }
    CHECK(most - least > 0.02 * volcano.height);
}

TEST_CASE("a volcano's slope is the change in its height", "[globe][volcanoes]") {
    const Planet planet;
    const hog::globe::Derived derived = hog::globe::derive(planet);
    const hog::globe::Generator::Shape shape;
    const hog::globe::terms::Context context{
        .planet = &planet, .derived = &derived, .shape = &shape};
    const Volcano volcano = first_volcano(planet);
    for (int k = 0; k < 24; ++k) {
        const double bearing = 0.4 + (k * 0.26);
        const DVec3 at = toward(planet, volcano, bearing, (0.05 + (0.04 * k)) * volcano.radius);
        const hog::globe::Noise here = hog::globe::terms::volcanoes(context, at, 0.5);
        // A step of two metres in a tangent direction, against the gradient along it.
        const DVec3 tangent = glm::normalize(glm::cross(at, DVec3{0.3, 0.5, 0.8}));
        constexpr double step = 0.01;
        const auto at_step = [&](double s) {
            return hog::globe::terms::volcanoes(
                       context, glm::normalize(at + (tangent * (s / planet.radius))), 0.5)
                .value;
        };
        const double numeric = (at_step(step) - at_step(-step)) / (2.0 * step);
        CHECK(std::abs(numeric - glm::dot(here.gradient, tangent)) < 2e-3 + (1e-3 * std::abs(numeric)));
    }
}
