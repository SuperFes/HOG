#include <hog/formation/rivers.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <cstdint>
#include <vector>

using Catch::Matchers::WithinRel;
using hog::formation::Body;
using hog::formation::Climate;
using hog::formation::climate_on;
using hog::formation::Crust;
using hog::formation::crust_on;
using hog::formation::Plates;
using hog::formation::Rivers;
using hog::formation::rivers_on;
using hog::formation::Sphere;

namespace {

struct Made {
    explicit Made(Body made_body = {}) : body{made_body} {}
    Body body;
    Sphere layout{5, body.seed};
    Sphere sphere{6, body.seed + 1};
    Plates plates{body, layout};
    Crust crust = crust_on(sphere, plates, body);
    Climate climate = climate_on(sphere, crust.base, crust.sea, body);
    Rivers rivers = rivers_on(sphere, crust, climate, body);
};

bool on_land(const Made& made, std::uint32_t point) {
    return made.crust.sea[point] == 0;
}

} // namespace

TEST_CASE("water from every land point reaches the sea, always downhill", "[formation][rivers]") {
    const Made made;
    const Rivers& rivers = made.rivers;
    for (std::uint32_t point = 0; point < made.sphere.size(); ++point) {
        if (!on_land(made, point)) {
            CHECK(rivers.receiver[point] == point);
            continue;
        }
        CHECK(rivers.height[point] > 0.0);
        std::uint32_t at = point;
        std::uint32_t steps = 0;
        while (on_land(made, at) && steps <= made.sphere.size()) {
            const std::uint32_t down = rivers.receiver[at];
            REQUIRE(down != at);
            if (on_land(made, down)) {
                REQUIRE(rivers.height[down] < rivers.height[at]);
            }
            at = down;
            ++steps;
        }
        CHECK_FALSE(on_land(made, at));
    }
}

TEST_CASE("all the rain on land comes out at the coasts", "[formation][rivers]") {
    const Made made;
    double fallen = 0.0;
    double out = 0.0;
    constexpr double earth_radius = 6'371'000.0;
    for (std::uint32_t point = 0; point < made.sphere.size(); ++point) {
        if (on_land(made, point)) {
            fallen += made.climate.rain[point] * made.sphere.area(point) * earth_radius *
                      earth_radius;
            if (!on_land(made, made.rivers.receiver[point])) {
                out += made.rivers.discharge[point];
            }
        }
    }
    CHECK_THAT(out, WithinRel(fallen, 1e-9));
}

TEST_CASE("the flow settles, and wear balances uplift where the crust doesn't hold the land down",
          "[formation][rivers]") {
    const Made made;
    // Settled before routing gives up at 200 rounds.
    CHECK(made.rivers.rounds < 200);
    int balanced = 0;
    int held = 0;
    for (std::uint32_t point = 0; point < made.sphere.size(); ++point) {
        CHECK(made.rivers.cover[point] >= 0.0);
        CHECK(made.rivers.cover[point] <= 1.0);
        const double wear = made.rivers.wear[point];
        const double uplift = made.crust.uplift[point];
        if (!on_land(made, point)) {
            CHECK(wear == 0.0);
            continue;
        }
        CHECK(wear >= 0.0);
        CHECK(wear <= uplift * (1.0 + 1e-12));
        if (made.rivers.height[point] < made.crust.base[point] - 1.0) {
            CHECK_THAT(wear, WithinRel(uplift, 1e-6));
            ++balanced;
        } else {
            ++held;
        }
    }
    CHECK(balanced > 0);
    CHECK(held > 0);
}

TEST_CASE("softer rock wears to lower land, and none stands over its crust", "[formation][rivers]") {
    Body soft;
    soft.erodibility *= 2.0;
    const Made firm;
    const Made softer{soft};
    const auto mean_land = [](const Made& made) {
        double sum = 0.0;
        int count = 0;
        for (std::uint32_t point = 0; point < made.sphere.size(); ++point) {
            if (on_land(made, point)) {
                sum += made.rivers.height[point];
                ++count;
                // Hollows are filled to just over the ground they drain to.
                const std::uint32_t down = made.rivers.receiver[point];
                const double below = on_land(made, down) ? made.rivers.height[down] : 0.0;
                CHECK(made.rivers.height[point] <= std::max(made.crust.base[point], below) + 1.0);
            }
        }
        return sum / count;
    };
    CHECK(mean_land(softer) < 0.9 * mean_land(firm));
}

TEST_CASE("the same body wears the same land", "[formation][rivers]") {
    const Made one;
    const Made two;
    CHECK(one.rivers.height == two.rivers.height);
    CHECK(one.rivers.receiver == two.rivers.receiver);
}
