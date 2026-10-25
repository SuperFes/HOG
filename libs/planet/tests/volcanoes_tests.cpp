#include <hog/planet/continents.hpp>
#include <hog/planet/noise.hpp>
#include <hog/planet/projection.hpp>
#include <hog/planet/volcanoes.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <memory>
#include <vector>

using hog::planet::Continents;
using hog::planet::Plates;
using hog::planet::Volcanoes;

namespace {

constexpr double radius = 115'000.0;
constexpr double spin = 50'000.0 / radius;

// Two plates grown from opposite sides of the planet, turning about the pole so they close over
// +y and part over -y, carrying the given crust.
Continents pair(double east_crust, double west_crust) {
    return Continents{
        1, 0.3,
        std::make_shared<const Plates>(
            1, hog::planet::LevelGrid{Plates::level, radius}, radius,
            std::vector<Plates::Plate>{
                {.origin = {1.0, 0.0, 0.0}, .spin = {0.0, 0.0, spin}, .continental = east_crust},
                {.origin = {-1.0, 0.0, 0.0},
                 .spin = {0.0, 0.0, -spin},
                 .continental = west_crust}})};
}

double apart(hog::DVec3 from, hog::DVec3 to) {
    return glm::length(to - from) * radius;
}

// `metres` from `from` towards `towards`.
hog::DVec3 along(hog::DVec3 from, hog::DVec3 towards, double metres) {
    const hog::DVec3 across = glm::normalize(towards - (from * glm::dot(towards, from)));
    return glm::normalize(from + (across * (metres / radius)));
}

double lift(const Volcanoes& volcanoes, hog::DVec3 at) {
    return hog::planet::fixed_to_double(volcanoes.lift(hog::planet::cell_at(at)));
}

} // namespace

TEST_CASE("volcanoes stand along arcs and nowhere else", "[planet][volcanoes]") {
    const Continents subducting = pair(0.8, -0.8);
    const Volcanoes found{1, radius, subducting};
    REQUIRE(found.volcanoes().size() >= 3);
    for (const auto& volcano : found.volcanoes()) {
        CHECK(hog::planet::fixed_to_double(
                  subducting.crust(hog::planet::cell_at(volcano.center)).arcs) > 0.15);
        // Where the plates close, inland on the east plate's continent.
        CHECK(volcano.center.y > 0.0);
        CHECK(volcano.center.x > 0.0);
    }
    const Continents colliding = pair(0.8, 0.8);
    CHECK(Volcanoes{1, radius, colliding}.volcanoes().empty());
    CHECK(lift(Volcanoes{1, radius, colliding}, {0.0, 1.0, 0.0}) == 0.0);
}

TEST_CASE("volcanoes keep apart and within their sizes", "[planet][volcanoes]") {
    const Continents subducting = pair(-0.8, -0.8);
    const Volcanoes found{1, radius, subducting};
    REQUIRE(found.volcanoes().size() >= 3);
    const auto volcanoes = found.volcanoes();
    for (std::size_t first = 0; first < volcanoes.size(); ++first) {
        const auto& volcano = volcanoes[first];
        CHECK(volcano.height >= Volcanoes::max_height / 2.0);
        CHECK(volcano.height <= Volcanoes::max_height);
        CHECK(volcano.radius == Catch::Approx(volcano.height / Volcanoes::flank_slope));
        for (std::size_t second = first + 1; second < volcanoes.size(); ++second) {
            CHECK(apart(volcano.center, volcanoes[second].center) > 3'000.0);
        }
    }
}

TEST_CASE("a volcano is a cone with a crater at its summit", "[planet][volcanoes]") {
    const Continents subducting = pair(0.8, -0.8);
    const Volcanoes found{1, radius, subducting};
    // One no other reaches, so its own shape shows.
    const Volcanoes::Volcano* alone = nullptr;
    for (const auto& volcano : found.volcanoes()) {
        bool crowded = false;
        for (const auto& other : found.volcanoes()) {
            crowded = crowded || (&other != &volcano && apart(volcano.center, other.center) <
                                                            volcano.radius + other.radius);
        }
        if (!crowded) {
            alone = &volcano;
            break;
        }
    }
    REQUIRE(alone != nullptr);
    const auto at = [&](hog::DVec3 out, double fraction) {
        return lift(found, along(alone->center, out, fraction * alone->radius));
    };
    // The rim, the highest anywhere, stands around the crater.
    const double floor = at({0.0, 0.0, 1.0}, 0.0);
    CHECK(floor == Catch::Approx(alone->height * ((0.93 * 1.93 / 2.0) - 0.06)).epsilon(0.02));
    for (const hog::DVec3 out : {hog::DVec3{0.0, 0.0, 1.0}, hog::DVec3{1.0, 0.0, 0.0},
                                 hog::DVec3{0.0, 0.0, -1.0}, hog::DVec3{-1.0, 0.0, 1.0}}) {
        CHECK(at(out, 0.03) > floor);
        // Down every way out, falling ever more gently to a foot the lobes move.
        double highest = 0.0;
        for (int step = 1; step <= 20; ++step) {
            highest = std::max(highest, at(out, step * 0.01));
        }
        CHECK(highest > floor + (alone->height * 0.05));
        CHECK(highest < alone->height);
        for (int step = 3; step < 35; ++step) {
            CHECK(at(out, (step + 1) * 0.04) <= at(out, step * 0.04));
        }
        CHECK(at(out, 0.2) - at(out, 0.3) > at(out, 0.6) - at(out, 0.7));
        CHECK(at(out, 1.0 - Volcanoes::lobes) > 0.0);
        CHECK(at(out, 1.0 / (1.0 - Volcanoes::lobes)) == 0.0);
    }
}

TEST_CASE("every volcano reaching a cell raises it", "[planet][volcanoes]") {
    const Continents subducting = pair(-0.8, -0.8);
    const Volcanoes found{1, radius, subducting};
    REQUIRE_FALSE(found.volcanoes().empty());
    // Around the arc over +y, west of the boundary.
    for (int x = -60; x <= 10; ++x) {
        for (int z = -30; z <= 30; ++z) {
            const hog::DVec3 at =
                glm::normalize(hog::DVec3{x * 1'000.0 / radius, 1.0, z * 1'000.0 / radius});
            const hog::DVec3 center = hog::planet::cell_center(hog::planet::cell_at(at));
            bool surely = false;
            bool maybe = false;
            for (const auto& volcano : found.volcanoes()) {
                const double reach = apart(center, volcano.center) / volcano.radius;
                surely = surely || reach < 1.0 - Volcanoes::lobes;
                maybe = maybe || reach < 1.0 / (1.0 - Volcanoes::lobes);
            }
            if (surely) {
                CHECK(lift(found, at) > 0.0);
            } else if (!maybe) {
                CHECK(lift(found, at) == 0.0);
            }
        }
    }
}

TEST_CASE("volcanoes depend on the seed alone", "[planet][volcanoes]") {
    const Continents subducting = pair(-0.8, -0.8);
    const Volcanoes first{1, radius, subducting};
    const Volcanoes again{1, radius, subducting};
    const Volcanoes other{2, radius, subducting};
    REQUIRE(first.volcanoes().size() == again.volcanoes().size());
    for (std::size_t index = 0; index < first.volcanoes().size(); ++index) {
        CHECK(first.volcanoes()[index].center == again.volcanoes()[index].center);
        CHECK(first.volcanoes()[index].height == again.volcanoes()[index].height);
    }
    REQUIRE_FALSE(other.volcanoes().empty());
    CHECK(other.volcanoes().front().center != first.volcanoes().front().center);
}
