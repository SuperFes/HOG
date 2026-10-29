#include <hog/waves/tide.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>
#include <span>
#include <vector>

using Catch::Matchers::WithinAbs;
using hog::DVec3;
using hog::waves::Pull;
using hog::waves::tide_at;

namespace {

constexpr double degree = std::numbers::pi / 180.0;

DVec3 up_at(double latitude, double longitude) {
    return {std::cos(latitude * degree) * std::cos(longitude * degree),
            std::cos(latitude * degree) * std::sin(longitude * degree),
            std::sin(latitude * degree)};
}

// The tide at `up` through a day, a degree of the planet's turn at a time: each body carried
// once round it.
std::vector<double> through_a_day(DVec3 up, std::span<const Pull> pulls) {
    std::vector<double> tides;
    for (int step = 0; step < 360; ++step) {
        const double turn = step * degree;
        std::vector<Pull> turned;
        for (const Pull& pull : pulls) {
            turned.push_back(
                {.toward = {(pull.toward.x * std::cos(turn)) - (pull.toward.y * std::sin(turn)),
                            (pull.toward.x * std::sin(turn)) + (pull.toward.y * std::cos(turn)),
                            pull.toward.z},
                 .height = pull.height});
        }
        tides.push_back(tide_at(up, turned));
    }
    return tides;
}

double spread(DVec3 up, std::span<const Pull> pulls) {
    const std::vector<double> tides = through_a_day(up, pulls);
    const auto [low, high] = std::ranges::minmax(tides);
    return high - low;
}

// The highs of the day, each where the tide stands above the hours either side.
std::vector<double> highs(DVec3 up, std::span<const Pull> pulls) {
    const std::vector<double> tides = through_a_day(up, pulls);
    std::vector<double> found;
    for (std::size_t hour = 0; hour < tides.size(); ++hour) {
        const double before = tides[(hour + tides.size() - 1) % tides.size()];
        const double after = tides[(hour + 1) % tides.size()];
        if (tides[hour] > before && tides[hour] >= after) {
            found.push_back(tides[hour]);
        }
    }
    return found;
}

} // namespace

TEST_CASE("a body draws the tide up twice a day", "[waves][tide]") {
    const std::array pulls{Pull{.toward = up_at(0.0, 40.0), .height = 0.6}};
    CHECK(highs(up_at(0.0, 0.0), pulls).size() == 2);
    CHECK(highs(up_at(35.0, 120.0), pulls).size() == 2);
}

TEST_CASE("the tide runs high at springs and low at neaps", "[waves][tide]") {
    const std::array springs{Pull{.toward = up_at(0.0, 10.0), .height = 0.6},
                             Pull{.toward = up_at(0.0, 10.0), .height = 0.28}};
    const std::array neaps{Pull{.toward = up_at(0.0, 10.0), .height = 0.6},
                           Pull{.toward = up_at(0.0, 100.0), .height = 0.28}};
    const DVec3 coast = up_at(0.0, 53.0);
    CHECK_THAT(spread(coast, springs) / spread(coast, neaps),
               WithinAbs((0.6 + 0.28) / (0.6 - 0.28), 0.05));
}

TEST_CASE("a body off the equator makes one of the day's tides the higher", "[waves][tide]") {
    const DVec3 coast = up_at(30.0, 0.0);
    const std::array declined{Pull{.toward = up_at(20.0, 0.0), .height = 0.6}};
    const std::vector<double> unequal = highs(coast, declined);
    REQUIRE(unequal.size() == 2);
    CHECK(std::abs(unequal[0] - unequal[1]) > 0.1);

    const std::array level{Pull{.toward = up_at(0.0, 0.0), .height = 0.6}};
    const std::vector<double> equal = highs(coast, level);
    REQUIRE(equal.size() == 2);
    CHECK_THAT(equal[0] - equal[1], WithinAbs(0.0, 1e-3));
}

TEST_CASE("the tide falls away towards the poles and is never far from the mean", "[waves][tide]") {
    const std::array pulls{Pull{.toward = up_at(25.0, 70.0), .height = 0.6},
                           Pull{.toward = up_at(-20.0, 200.0), .height = 0.28}};
    // The equilibrium tide's daily parts reach 5/4 of a body's height, and the coasts half again.
    constexpr double most = 1.5 * 1.25 * (0.6 + 0.28);
    for (int at = 0; at < 50; ++at) {
        const DVec3 up =
            glm::normalize(DVec3{std::cos(at * 1.3), std::sin(at * 0.7), (at % 7) - 3.0});
        CHECK(std::abs(tide_at(up, pulls)) <= most);
    }
    CHECK_THAT(tide_at({0.0, 0.0, 1.0}, pulls), WithinAbs(0.0, 1e-12));
    CHECK(spread(up_at(89.5, 0.0), pulls) < 0.05 * spread(up_at(0.0, 0.0), pulls));
    CHECK(tide_at(up_at(10.0, 10.0), {}) == 0.0);
}

TEST_CASE("the tide changes smoothly along the coast", "[waves][tide]") {
    // A hundred metres along a planet of a few hundred kilometres' radius.
    constexpr double step = 100.0 / 300'000.0;
    const DVec3 coast{0.6, 0.8, 0.0};
    const DVec3 along = glm::normalize(coast + DVec3{0.0, 0.0, step});
    for (int hour = 0; hour < 24; ++hour) {
        const double turn = hour * 15.0;
        const std::array pulls{Pull{.toward = up_at(18.0, turn), .height = 0.6},
                               Pull{.toward = up_at(-23.0, turn * 1.03), .height = 0.28}};
        CHECK(std::abs(tide_at(coast, pulls) - tide_at(along, pulls)) < 0.005);
    }
}
