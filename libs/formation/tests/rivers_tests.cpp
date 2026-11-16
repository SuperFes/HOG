#include <hog/formation/rivers.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <cmath>
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
    const double radius = made.body.radius;
    for (std::uint32_t point = 0; point < made.sphere.size(); ++point) {
        if (on_land(made, point)) {
            fallen += made.climate.rain[point] * made.sphere.area(point) * radius * radius;
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

TEST_CASE("softer rock wears to lower land, held down by its crust", "[formation][rivers]") {
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
                // Hollows are filled to just over the ground they drain to, and the crust's float
                // holds the land down to within ten times its held relief of it.
                const std::uint32_t down = made.rivers.receiver[point];
                const double below = on_land(made, down) ? made.rivers.height[down] : 0.0;
                CHECK(made.rivers.height[point] <=
                      std::max(made.crust.base[point] + (10.0 * made.body.held_relief), below) +
                          1.0);
            }
        }
        return sum / count;
    };
    CHECK(mean_land(softer) < 0.9 * mean_land(firm));
}

TEST_CASE("strong rock stands higher than weak, and middling rock as if unknown",
          "[formation][rivers]") {
    const Made made;
    const std::vector<double> weak(made.sphere.size(), 0.0);
    const std::vector<double> middling(made.sphere.size(), 0.5);
    const std::vector<double> strong(made.sphere.size(), 1.0);
    const auto mean_land = [&](const Rivers& rivers) {
        double sum = 0.0;
        int count = 0;
        for (std::uint32_t point = 0; point < made.sphere.size(); ++point) {
            if (on_land(made, point)) {
                sum += rivers.height[point];
                ++count;
            }
        }
        return sum / count;
    };
    const Rivers on_weak = rivers_on(made.sphere, made.crust, made.climate, weak, made.body);
    const Rivers on_strong = rivers_on(made.sphere, made.crust, made.climate, strong, made.body);
    CHECK(mean_land(on_strong) > mean_land(on_weak));
    const Rivers on_middling =
        rivers_on(made.sphere, made.crust, made.climate, middling, made.body);
    CHECK(on_middling.height == made.rivers.height);
}

TEST_CASE("the same body wears the same land", "[formation][rivers]") {
    const Made one;
    const Made two;
    CHECK(one.rivers.height == two.rivers.height);
    CHECK(one.rivers.receiver == two.rivers.receiver);
}

TEST_CASE("a titan belt's rock stands steeper, its land higher than ordinary rock's",
          "[formation][rivers]") {
    Body plain;
    plain.titan_ranges = 0;
    const Made titan;
    const Made usual{plain};
    double titan_highest = 0.0;
    double usual_highest = 0.0;
    for (std::uint32_t point = 0; point < titan.sphere.size(); ++point) {
        if (titan.crust.titan[point] > 0.5 && on_land(titan, point)) {
            titan_highest = std::max(titan_highest, titan.rivers.height[point]);
            usual_highest = std::max(usual_highest, usual.rivers.height[point]);
        }
    }
    CHECK(titan_highest > 1.25 * usual_highest);
}

TEST_CASE("land its crust's float holds down keeps its relief, never a level plain",
          "[formation][rivers]") {
    Body flat;
    flat.held_relief = 0.0;
    flat.plain_relief = 0.0;
    const Made made;
    const Made capped{flat};
    // Land whose neighbours on land all stand within a metre of it, of land held down.
    const auto level_share = [](const Made& world) {
        int held = 0;
        int level = 0;
        for (std::uint32_t point = 0; point < world.sphere.size(); ++point) {
            if (!on_land(world, point) ||
                world.rivers.height[point] < world.crust.base[point] - 1.0) {
                continue;
            }
            ++held;
            bool flat_about = true;
            for (const std::uint32_t next : world.sphere.neighbors(point)) {
                if (on_land(world, next) &&
                    std::abs(world.rivers.height[next] - world.rivers.height[point]) > 1.0) {
                    flat_about = false;
                }
            }
            level += flat_about ? 1 : 0;
        }
        UNSCOPED_INFO("held " << held << ", level " << level);
        return held > 0 ? static_cast<double>(level) / held : 0.0;
    };
    CHECK(level_share(capped) > 0.1);
    CHECK(level_share(made) < 0.02);
}

TEST_CASE("rivers on old plains wander rather than run as straight as the slope they start on",
          "[formation][rivers]") {
    Body level;
    level.plain_relief = 0.0;
    const Made made;
    const Made smooth{level};
    // How far rivers on light crust away from belts get over a reach, against the way they ran.
    constexpr int reach = 8;
    const auto straightness = [](const Made& world) {
        double straight = 0.0;
        int rivers = 0;
        for (std::uint32_t point = 0; point < world.sphere.size(); ++point) {
            if (!on_land(world, point) || world.crust.light[point] < 1.0 ||
                world.crust.meeting[point] != hog::formation::Meeting::None) {
                continue;
            }
            std::uint32_t at = point;
            double path = 0.0;
            int steps = 0;
            while (steps < reach && on_land(world, world.rivers.receiver[at])) {
                const std::uint32_t down = world.rivers.receiver[at];
                path += glm::length(world.sphere.point(down) - world.sphere.point(at));
                at = down;
                ++steps;
            }
            if (steps == reach) {
                straight += glm::length(world.sphere.point(at) - world.sphere.point(point)) / path;
                ++rivers;
            }
        }
        UNSCOPED_INFO(rivers << " reaches");
        return rivers > 0 ? straight / rivers : 1.0;
    };
    const double wandering = straightness(made);
    const double straight = straightness(smooth);
    UNSCOPED_INFO("chord over path " << wandering << " with relief, " << straight << " without");
    CHECK(wandering < straight - 0.02);
}
