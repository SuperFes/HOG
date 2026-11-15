#include <hog/formation/plates.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <numbers>
#include <vector>

using Catch::Matchers::WithinAbs;
using hog::formation::bending_reach;
using hog::formation::Body;
using hog::formation::Crust;
using hog::formation::crust_on;
using hog::formation::Meeting;
using hog::formation::Plates;
using hog::formation::Sphere;

TEST_CASE("every point of the layout lies on a plate", "[formation][plates]") {
    const Body body;
    const Sphere layout{5, body.seed};
    const Plates plates{body, layout};
    CHECK(plates.count() == body.plates);
    for (std::uint32_t point = 0; point < layout.size(); ++point) {
        CHECK(plates.plate(point) < plates.count());
    }
}

TEST_CASE("light crust covers about the share asked for", "[formation][plates]") {
    Body body;
    body.land_share = GENERATE(0.1, 0.3, 0.6);
    const Sphere layout{5, body.seed};
    const Plates plates{body, layout};
    double area = 0.0;
    for (std::uint32_t point = 0; point < layout.size(); ++point) {
        area += plates.light(point) ? layout.area(point) : 0.0;
    }
    CHECK_THAT(area / (4.0 * std::numbers::pi), WithinAbs(body.land_share, 0.01));
}

TEST_CASE("plates move along the surface, and pairs agree on their boundary",
          "[formation][plates]") {
    const Body body;
    const Sphere layout{4, body.seed};
    const Plates plates{body, layout};
    for (int one = 0; one < plates.count(); ++one) {
        const auto a = static_cast<std::uint16_t>(one);
        CHECK_THAT(glm::dot(plates.velocity(a, layout.point(a)), layout.point(a)),
                   WithinAbs(0.0, 1e-15));
        for (int other = 0; other < plates.count(); ++other) {
            const auto b = static_cast<std::uint16_t>(other);
            CHECK(plates.age(a, b) == plates.age(b, a));
            if (a != b) {
                CHECK(plates.rides_over(a, b) != plates.rides_over(b, a));
            }
        }
    }
}

namespace {

struct Made {
    explicit Made(Body made_body = {}) : body{made_body} {}
    Body body;
    Sphere layout{5, body.seed};
    Sphere sphere{6, body.seed + 1};
    Plates plates{body, layout};
    Crust crust = crust_on(sphere, plates, body);
};

} // namespace

TEST_CASE("crust stands high where light and deep where sea floor is old", "[formation][plates]") {
    const Made made;
    const Crust& crust = made.crust;
    double land = 0.0;
    double high = 0.0;
    double low = 0.0;
    int highs = 0;
    int lows = 0;
    for (std::uint32_t point = 0; point < made.sphere.size(); ++point) {
        CHECK(crust.light[point] >= 0.0);
        CHECK(crust.light[point] <= 1.0);
        CHECK(crust.uplift[point] >= 0.0);
        if (crust.light[point] >= 0.5) {
            land += made.sphere.area(point);
            high += crust.base[point];
            ++highs;
        } else {
            low += crust.base[point];
            ++lows;
        }
    }
    CHECK_THAT(land / (4.0 * std::numbers::pi), WithinAbs(made.body.land_share, 0.05));
    REQUIRE(highs > 0);
    REQUIRE(lows > 0);
    CHECK(high / highs > 0.0);
    CHECK(low / lows < made.body.ridge_depth);
}

TEST_CASE("boundaries of each kind form, and belts stand high", "[formation][plates]") {
    const Made made;
    const Crust& crust = made.crust;
    std::vector<int> kinds(8, 0);
    double collision_height = 0.0;
    int collisions = 0;
    for (std::uint32_t point = 0; point < made.sphere.size(); ++point) {
        ++kinds[static_cast<std::size_t>(crust.meeting[point])];
        if (crust.meeting[point] == Meeting::Collision && crust.boundary[point] < 0.01) {
            collision_height += crust.base[point];
            ++collisions;
        }
    }
    CHECK(kinds[static_cast<std::size_t>(Meeting::Ridge)] > 0);
    CHECK(kinds[static_cast<std::size_t>(Meeting::Transform)] > 0);
    CHECK(kinds[static_cast<std::size_t>(Meeting::Subduction)] +
              kinds[static_cast<std::size_t>(Meeting::IslandArc)] >
          0);
    if (collisions > 0) {
        CHECK(collision_height / collisions > made.body.continent_height);
    }
}

TEST_CASE("sea floor is young at the ridges", "[formation][plates]") {
    const Made made;
    const Crust& crust = made.crust;
    for (std::uint32_t point = 0; point < made.sphere.size(); ++point) {
        if (crust.meeting[point] == Meeting::Ridge && crust.boundary[point] < 0.005) {
            CHECK(crust.floor_age[point] < 20.0);
        }
    }
}

TEST_CASE("light crust thins across its whole margin, holding up patches narrower",
          "[formation][plates]") {
    Body body;
    const Sphere layout{5, body.seed};
    const Sphere sphere{6, body.seed + 1};
    // Sized so the margin spans several points, and the coasts' wiggles finer than it.
    body.radius = std::numbers::pi * bending_reach(body) / (6.0 * sphere.spacing());
    const Plates plates{body, layout};
    const Crust crust = crust_on(sphere, plates, body);
    double most = 0.0;
    for (std::uint32_t point = 0; point < sphere.size(); ++point) {
        double around = 0.0;
        for (const std::uint32_t next : sphere.neighbors(point)) {
            around += crust.light[next];
        }
        around /= static_cast<double>(sphere.neighbors(point).size());
        most = std::max(most, std::abs(crust.light[point] - around));
    }
    CHECK(most < 0.1);
}

TEST_CASE("the same body makes the same crust", "[formation][plates]") {
    const Made one;
    const Made two;
    CHECK(one.crust.base == two.crust.base);
    CHECK(one.crust.uplift == two.crust.uplift);
}

TEST_CASE("a titan belt holds far thicker crust, along one pair of plates only",
          "[formation][plates]") {
    Body plain;
    plain.titan_ranges = 0;
    const Sphere layout{5, plain.seed};
    const Plates without{plain, layout};
    const Plates with{Body{}, layout};
    std::vector<std::uint32_t> pairs;
    for (std::size_t index = 0; index < with.boundaries().size(); ++index) {
        const auto& titan = with.boundaries()[index];
        const auto& usual = without.boundaries()[index];
        CHECK_FALSE(usual.titan);
        if (!titan.titan) {
            CHECK(titan.thickened == usual.thickened);
            continue;
        }
        CHECK((titan.meeting == Meeting::Collision || titan.meeting == Meeting::Subduction));
        CHECK(titan.thickened > 2.0 * usual.thickened);
        const auto key =
            (std::uint32_t{std::min(titan.a, titan.b)} << 16U) | std::max(titan.a, titan.b);
        if (std::ranges::find(pairs, key) == pairs.end()) {
            pairs.push_back(key);
        }
    }
    CHECK(pairs.size() == 1);
}

TEST_CASE("a belt's crest rises and dips along it rather than running level",
          "[formation][plates]") {
    Body level;
    level.belt_ruggedness = 0.0;
    const Body rugged;
    const Sphere layout{5, level.seed};
    const Sphere sphere{6, level.seed + 1};
    const Plates plates{level, layout};
    const Crust flat = crust_on(sphere, plates, level);
    const Crust rough = crust_on(sphere, plates, rugged);
    double sum = 0.0;
    double squares = 0.0;
    int belts = 0;
    for (std::uint32_t point = 0; point < sphere.size(); ++point) {
        // Thickened light crust, so all it stands over the continents' height is the belt's.
        const double thickened = flat.base[point] - level.continent_height;
        if (flat.meeting[point] != Meeting::Collision || flat.light[point] < 0.999 ||
            thickened < 500.0) {
            continue;
        }
        const double ratio = (rough.base[point] - flat.base[point]) / thickened;
        CHECK(std::abs(ratio) <= rugged.belt_ruggedness);
        sum += ratio;
        squares += ratio * ratio;
        ++belts;
    }
    REQUIRE(belts > 50);
    const double mean = sum / belts;
    CHECK(std::sqrt((squares / belts) - (mean * mean)) > 0.05);
}

TEST_CASE("old continental crust sags into broad basins and bows up into swells",
          "[formation][plates]") {
    Body level;
    level.continent_swell = 0.0;
    const Made made;
    const Made flat{level};
    double lowest = 0.0;
    double highest = 0.0;
    double steepest = 0.0;
    const auto swell = [&](std::uint32_t point) {
        return made.crust.base[point] - flat.crust.base[point];
    };
    for (std::uint32_t point = 0; point < made.sphere.size(); ++point) {
        lowest = std::min(lowest, swell(point));
        highest = std::max(highest, swell(point));
        for (const std::uint32_t next : made.sphere.neighbors(point)) {
            if (made.crust.light[point] < 1.0 || made.crust.light[next] < 1.0) {
                continue;
            }
            const double run =
                glm::length(made.sphere.point(point) - made.sphere.point(next)) * made.body.radius;
            steepest = std::max(steepest, std::abs(swell(next) - swell(point)) / run);
        }
        // Only light crust swells.
        if (made.crust.light[point] == 0.0) {
            CHECK(swell(point) == 0.0);
        }
    }
    // Swells are as high as a thousandth of their widths allows on a world this small.
    const double narrowest = 1e-3 * 0.05 * made.body.radius;
    CHECK(highest > narrowest);
    CHECK(lowest < -narrowest);
    CHECK(highest - lowest < 2.0 * made.body.continent_height * made.body.continent_swell);
    CHECK(steepest < 2e-3);
}
