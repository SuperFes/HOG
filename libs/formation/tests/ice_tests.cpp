#include <hog/formation/ice.hpp>
#include <hog/formation/plates.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <numbers>

using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;
using hog::formation::Body;
using hog::formation::Climate;
using hog::formation::climate_on;
using hog::formation::cold_share;
using hog::formation::Crust;
using hog::formation::crust_on;
using hog::formation::deepest_dip;
using hog::formation::degree_days;
using hog::formation::Ice;
using hog::formation::ice_on;
using hog::formation::ice_shaped;
using hog::formation::ice_thickness;
using hog::formation::Plates;
using hog::formation::Rivers;
using hog::formation::rivers_on;
using hog::formation::seasonal_swing;
using hog::formation::Sphere;

TEST_CASE("a year spends degree-days over 0 as its mean and swing allow", "[formation][ice]") {
    CHECK_THAT(degree_days(5.0, 0.0), WithinRel(5.0 * 365.25, 1e-12));
    CHECK_THAT(degree_days(12.0, 10.0), WithinRel(12.0 * 365.25, 1e-12));
    CHECK(degree_days(-12.0, 10.0) == 0.0);
    CHECK_THAT(degree_days(0.0, 10.0), WithinRel(365.25 * 10.0 / std::numbers::pi, 1e-9));
    // Meets the whole-year and never-thawing ends smoothly.
    CHECK_THAT(degree_days(9.999, 10.0), WithinRel(9.999 * 365.25, 1e-4));
    CHECK(degree_days(-9.999, 10.0) < 1e-2);
    CHECK(degree_days(-2.0, 10.0) > 0.0);
    CHECK(degree_days(-1.0, 10.0) > degree_days(-2.0, 10.0));
}

TEST_CASE("a year spends a share below 0 as its mean and swing allow", "[formation][ice]") {
    CHECK_THAT(cold_share(0.0, 10.0), WithinAbs(0.5, 1e-12));
    CHECK(cold_share(10.0, 10.0) == 0.0);
    CHECK(cold_share(-10.0, 10.0) == 1.0);
    CHECK(cold_share(-3.0, 10.0) > cold_share(3.0, 10.0));
    CHECK(cold_share(-1.0, 0.0) == 1.0);
    CHECK(cold_share(1.0, 0.0) == 0.0);
}

TEST_CASE("years swing most near the poles, over land, with tilt", "[formation][ice]") {
    const Body body;
    CHECK_THAT(seasonal_swing(0.0, false, body), WithinAbs(0.0, 1e-12));
    CHECK(seasonal_swing(1.4, false, body) > seasonal_swing(0.6, false, body));
    CHECK(seasonal_swing(1.0, true, body) < seasonal_swing(1.0, false, body));
    Body upright;
    upright.tilt = 0.0;
    CHECK_THAT(seasonal_swing(1.0, false, upright), WithinAbs(0.0, 1e-12));
}

TEST_CASE("plastic ice thickens up from where it ends", "[formation][ice]") {
    const Body body;
    const double held = 60'000.0 * body.ice_strength / (body.ice_density * body.gravity);
    const double flat = ice_thickness(100.0, 100.0, 60'000.0, body);
    CHECK_THAT(flat, WithinRel(std::sqrt(held), 1e-9));
    for (const double bed : {-500.0, 0.0, 300.0, 3'000.0}) {
        const double thick = ice_thickness(bed, 200.0, 60'000.0, body);
        CHECK(thick > 0.0);
        CHECK_THAT(thick * (bed + thick - 200.0), WithinRel(held, 1e-9));
    }
    CHECK(ice_thickness(0.0, 0.0, 120'000.0, body) > flat);
    CHECK(ice_thickness(3'000.0, 0.0, 60'000.0, body) < flat);
}

namespace {

struct Made {
    explicit Made(Body made_with = {}) : body{made_with} {}
    Body body;
    Sphere layout{5, body.seed};
    Sphere sphere{6, body.seed + 1};
    Plates plates{body, layout};
    Crust crust = crust_on(sphere, plates, body);
    Climate climate = climate_on(sphere, crust.base, crust.sea, body);
    Rivers rivers = rivers_on(sphere, crust, climate, body);
    Ice ice = ice_on(sphere, crust, climate, rivers, body);
};

double iced_area(const Sphere& sphere, const std::vector<double>& thickness) {
    double area = 0.0;
    for (std::uint32_t point = 0; point < sphere.size(); ++point) {
        area += thickness[point] > 0.0 ? sphere.area(point) : 0.0;
    }
    return area;
}

} // namespace

TEST_CASE("ice lies only where snow outlasts summers or ice flows in", "[formation][ice]") {
    const Made made;
    CHECK(iced_area(made.sphere, made.ice.thickness) > 0.0);
    CHECK(made.ice.calving >= 0.0);
    for (std::uint32_t point = 0; point < made.sphere.size(); ++point) {
        CHECK(made.ice.flux[point] >= 0.0);
        CHECK(made.ice.carved[point] >= 0.0);
        if (made.ice.flux[point] > 0.0) {
            CHECK(made.ice.thickness[point] > 0.0);
        }
        if (made.ice.carved[point] > 0.0) {
            CHECK(made.ice.coldest[point] > 0.0);
        }
    }
}

TEST_CASE("the coldest times laid more ice than lies now, and it carved", "[formation][ice]") {
    const Made made;
    CHECK(iced_area(made.sphere, made.ice.coldest) > iced_area(made.sphere, made.ice.thickness));
    CHECK(std::ranges::max(made.ice.carved) > 0.0);
}

TEST_CASE("a dimmer star ices more of the land, a brighter one less", "[formation][ice]") {
    const Made earthlike;
    Body dim;
    dim.star_flux = 1'300.0;
    Body bright;
    bright.star_flux = 1'450.0;
    const double usual = iced_area(earthlike.sphere, earthlike.ice.thickness);
    CHECK(iced_area(earthlike.sphere, Made{dim}.ice.thickness) > usual);
    CHECK(iced_area(earthlike.sphere, Made{bright}.ice.thickness) < usual);
}

TEST_CASE("the sea freezes near the poles and not at the equator", "[formation][ice]") {
    const Made made;
    double polar = 0.0;
    for (std::uint32_t point = 0; point < made.sphere.size(); ++point) {
        if (made.rivers.receiver[point] != point) {
            continue;
        }
        const double z = std::abs(made.sphere.point(point).z);
        if (z < 0.2) {
            CHECK(made.ice.sea_ice[point] == 0.0);
        }
        polar = std::max(polar, z > 0.95 ? made.ice.sea_ice[point] : 0.0);
    }
    CHECK(polar > 0.5);
}

TEST_CASE("water still finds its way down the land ice shaped", "[formation][ice]") {
    const Made made;
    const Rivers shaped = ice_shaped(made.sphere, made.rivers, made.ice);
    for (std::uint32_t point = 0; point < made.sphere.size(); ++point) {
        if (made.rivers.receiver[point] == point) {
            CHECK(shaped.receiver[point] == point);
            continue;
        }
        const double ground = made.rivers.ground[point] - made.ice.carved[point] +
                              made.ice.till[point] + made.ice.thickness[point];
        CHECK_THAT(shaped.ground[point], WithinAbs(ground, 1e-9));
        if (shaped.receiver[point] == point) {
            // The sea only comes into troughs below its level that open onto it.
            CHECK(shaped.ground[point] < 0.0);
            continue;
        }
        const std::uint32_t down = shaped.receiver[point];
        const double below = shaped.receiver[down] == down ? 0.0 : shaped.height[down];
        CHECK(shaped.height[point] > below);
        CHECK(shaped.height[point] >= shaped.ground[point]);
    }
}

TEST_CASE("water rerouted round the ice all still reaches the sea, filling only true hollows",
          "[formation][ice]") {
    const Made made;
    const Rivers shaped = ice_shaped(made.sphere, made.rivers, made.ice);
    const auto into_sea = [&](const Rivers& rivers) {
        double reaching = 0.0;
        for (std::uint32_t point = 0; point < made.sphere.size(); ++point) {
            if (rivers.receiver[point] != point &&
                rivers.receiver[rivers.receiver[point]] == rivers.receiver[point]) {
                reaching += rivers.discharge[point];
            }
        }
        return reaching;
    };
    // Only the rain falling where the sea came into troughs is the sea's now.
    std::vector<double> own = made.rivers.discharge;
    for (std::uint32_t point = 0; point < made.sphere.size(); ++point) {
        if (made.rivers.receiver[point] != point) {
            own[made.rivers.receiver[point]] -= made.rivers.discharge[point];
        }
    }
    double drowned = 0.0;
    for (std::uint32_t point = 0; point < made.sphere.size(); ++point) {
        const bool was_land = made.rivers.receiver[point] != point;
        drowned += was_land && shaped.receiver[point] == point ? own[point] : 0.0;
    }
    CHECK_THAT(into_sea(shaped) + drowned, WithinRel(into_sea(made.rivers), 1e-9));
    for (std::uint32_t point = 0; point < made.sphere.size(); ++point) {
        if (shaped.receiver[point] == point) {
            continue;
        }
        // A point lifted over its ground stands no higher than its lowest neighbour, plus the
        // faint fall water needs across a filled hollow.
        double lowest = std::numeric_limits<double>::infinity();
        for (const std::uint32_t next : made.sphere.neighbors(point)) {
            lowest = std::min(lowest, shaped.receiver[next] == next ? 0.0 : shaped.height[next]);
        }
        CHECK(shaped.height[point] <= std::max(shaped.ground[point], lowest + 1.0));
    }
}

TEST_CASE("all the rock ice carved is laid down as till or washed off", "[formation][ice]") {
    const Made made;
    constexpr double earth_radius = 6'371'000.0;
    double carved = 0.0;
    double laid = 0.0;
    double washed = 0.0;
    for (std::uint32_t point = 0; point < made.sphere.size(); ++point) {
        CHECK(made.ice.till[point] >= 0.0);
        CHECK(made.ice.outwash[point] >= 0.0);
        carved += made.ice.carved[point] * made.sphere.area(point);
        laid += made.ice.till[point] * made.sphere.area(point);
        washed += made.ice.outwash[point] / (earth_radius * earth_radius);
    }
    CHECK(carved > 0.0);
    CHECK(washed > 0.0);
    CHECK_THAT(laid + washed, WithinRel(carved, 1e-9));
}

TEST_CASE("till lies where the coldest ice lay, thickest where it ended", "[formation][ice]") {
    const Made made;
    const auto iced = [&](std::uint32_t point) {
        return made.rivers.receiver[point] != point && made.ice.coldest[point] > 0.0;
    };
    double edge = 0.0;
    double inner = 0.0;
    int edges = 0;
    int inners = 0;
    for (std::uint32_t point = 0; point < made.sphere.size(); ++point) {
        const bool land = made.rivers.receiver[point] != point;
        const auto neighbors = made.sphere.neighbors(point);
        if (land && made.ice.till[point] > 0.0) {
            CHECK((iced(point) || std::ranges::any_of(neighbors, iced)));
        }
        if (!iced(point)) {
            continue;
        }
        const bool ends = std::ranges::any_of(neighbors, [&](std::uint32_t next) {
            return made.rivers.receiver[next] != next && !iced(next);
        });
        (ends ? edge : inner) += made.ice.till[point];
        ++(ends ? edges : inners);
    }
    REQUIRE(edges > 0);
    REQUIRE(inners > 0);
    CHECK(edge / edges > inner / inners);
}

TEST_CASE("a bed dips under the bed down-ice only as far as the ice's fall allows",
          "[formation][ice]") {
    const Body body;
    const double k = body.overdeepening;
    const double held = 60'000.0 * body.ice_strength / (body.ice_density * body.gravity);
    for (const double ice_below : {0.0, 500.0, 2'000.0}) {
        const double dip = deepest_dip(ice_below, 60'000.0, body);
        CHECK(dip > 0.0);
        // Ice over the dip stands as plastic ice does, its top falling 1/k as steeply as the bed
        // rises towards the margin.
        const double over = ice_below + dip + (dip / k);
        CHECK_THAT(over * (over - ice_below - dip), WithinRel(held, 1e-9));
    }
    CHECK(deepest_dip(2'000.0, 60'000.0, body) < deepest_dip(0.0, 60'000.0, body));
}
