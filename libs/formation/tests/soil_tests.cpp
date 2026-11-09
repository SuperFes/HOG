#include <hog/formation/plates.hpp>
#include <hog/formation/soil.hpp>
#include <hog/formation/world.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>

using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;
using hog::DVec3;
using hog::formation::balanced_depth;
using hog::formation::Body;
using hog::formation::Climate;
using hog::formation::climate_on;
using hog::formation::Crust;
using hog::formation::crust_on;
using hog::formation::gathered;
using hog::formation::grown_depth;
using hog::formation::Ice;
using hog::formation::ice_on;
using hog::formation::ice_shaped;
using hog::formation::Lakes;
using hog::formation::lakes_on;
using hog::formation::Layer;
using hog::formation::layer_at;
using hog::formation::Parent;
using hog::formation::plant_growth;
using hog::formation::Plates;
using hog::formation::Profile;
using hog::formation::profile_at;
using hog::formation::Rivers;
using hog::formation::rivers_on;
using hog::formation::rotting_rate;
using hog::formation::Sediment;
using hog::formation::sediment_on;
using hog::formation::Soil;
using hog::formation::soil_making;
using hog::formation::soil_on;
using hog::formation::Sphere;
using hog::formation::waterlogged;
using hog::formation::wetness;

TEST_CASE("rock weathers faster warm and wet, not at all frozen", "[formation][soil]") {
    const Body body;
    CHECK(soil_making(25.0, 2.0, body) > soil_making(10.0, 2.0, body));
    CHECK(soil_making(15.0, 2.0, body) > soil_making(15.0, 0.1, body));
    CHECK(soil_making(-40.0, 2.0, body) < 1e-3 * soil_making(15.0, 2.0, body));
    // Frost shatters rock best a few degrees under freezing.
    CHECK(soil_making(-5.0, 0.3, body) > soil_making(-15.0, 0.3, body));
    CHECK(soil_making(-5.0, 0.3, body) > soil_making(-5.0, 0.0, body));
    CHECK(soil_making(15.0, 0.0, body) == 0.0);
}

TEST_CASE("soil lies as deep as making balances wear", "[formation][soil]") {
    const Body body;
    const double making = 1.0e-4;
    const double depth = balanced_depth(making, 1.0e-5, body);
    // At that depth, rock under it turns to soil exactly as fast as the ground wears.
    CHECK_THAT(making * std::exp(-body.soil_falloff * depth), WithinRel(1.0e-5, 1e-9));
    CHECK(balanced_depth(making, 2.0e-4, body) == 0.0);
    CHECK(balanced_depth(making, 0.0, body) == balanced_depth(making, body.slowest_wear, body));
}

TEST_CASE("soil grows from bare ground towards the depth that balances wear", "[formation][soil]") {
    const Body body;
    const double making = 1.0e-4;
    const double wear = 1.0e-5;
    CHECK(grown_depth(making, wear, 0.0, body) == 0.0);
    CHECK(grown_depth(making, wear, 1'000.0, body) < grown_depth(making, wear, 10'000.0, body));
    CHECK_THAT(grown_depth(making, wear, 1.0e8, body),
               WithinRel(balanced_depth(making, wear, body), 1e-9));
    CHECK(grown_depth(making, 2.0e-4, 1.0e8, body) == 0.0);
    // It grows as fast as bare ground turns to soil, slowing as the soil thickens.
    const double years = 5'000.0;
    const double depth = grown_depth(making, wear, years, body);
    const double rate = (grown_depth(making, wear, years + 1.0, body) -
                         grown_depth(making, wear, years - 1.0, body)) /
                        2.0;
    CHECK_THAT(rate, WithinRel((making * std::exp(-body.soil_falloff * depth)) - wear, 1e-5));
    Body still = body;
    still.slowest_wear = 0.0;
    CHECK_THAT(grown_depth(making, 0.0, years, still),
               WithinRel(std::log1p(body.soil_falloff * making * years) / body.soil_falloff, 1e-9));
}

TEST_CASE("fast-rising mountains are bare and still lowlands soiled", "[formation][soil]") {
    const Body body;
    const Sphere layout{5, body.seed};
    const Sphere sphere{6, body.seed + 1};
    const Plates plates{body, layout};
    const auto crust = crust_on(sphere, plates, body);
    const Climate climate = climate_on(sphere, crust.base, crust.sea, body);
    const Rivers rivers = rivers_on(sphere, crust, climate, body);
    const Soil soil = soil_on(sphere, rivers, climate, body);
    int soiled = 0;
    int land = 0;
    for (std::uint32_t point = 0; point < sphere.size(); ++point) {
        CHECK(soil.depth[point] >= 0.0);
        if (rivers.receiver[point] == point) {
            CHECK(soil.depth[point] == 0.0);
            continue;
        }
        ++land;
        soiled += soil.depth[point] > 0.0 ? 1 : 0;
        if (rivers.wear[point] > soil.making[point]) {
            CHECK(soil.depth[point] == 0.0);
        }
    }
    REQUIRE(land > 0);
    CHECK(soiled > land / 2);
}

namespace {

// A planet lit dimmer than the default, so its coldest times laid ice sheets wide enough to have
// insides and edges.
Body glaciated() {
    Body body;
    body.star_flux = 1'300.0;
    return body;
}

struct Made {
    explicit Made(Body made_with = {}) : body{made_with} {}
    Body body;
    Sphere layout{5, body.seed};
    Sphere sphere{6, body.seed + 1};
    Plates plates{body, layout};
    Crust crust = crust_on(sphere, plates, body);
    Climate climate = climate_on(sphere, crust.base, crust.sea, body);
    Rivers worn = rivers_on(sphere, crust, climate, body);
    Ice ice = ice_on(sphere, crust, climate, worn, body);
    Rivers rivers = ice_shaped(sphere, worn, ice);
    Lakes lakes = lakes_on(sphere, rivers, climate, body);
    Sediment sediment = sediment_on(sphere, rivers, lakes, ice.outwash, body);
    Soil soil = soil_on(sphere, rivers, climate, ice, lakes, sediment, body);
    Soil on_rock = soil_on(sphere, rivers, climate, body);
};

} // namespace

TEST_CASE("no soil forms under ice, standing water or salt", "[formation][soil]") {
    const Made made;
    for (std::uint32_t point = 0; point < made.sphere.size(); ++point) {
        const bool covered =
            made.rivers.receiver[point] == point || made.ice.thickness[point] > 0.0 ||
            made.sediment.lake_depth[point] > 0.0 || made.lakes.salt_flat[point] != 0;
        CHECK((made.soil.parent[point] == Parent::None) == covered);
        if (covered) {
            CHECK(made.soil.depth[point] == 0.0);
        }
    }
}

TEST_CASE("soil grows down through what was laid, faster than into rock", "[formation][soil]") {
    const Made made;
    int on_till = 0;
    int on_alluvium = 0;
    int on_lake_beds = 0;
    for (std::uint32_t point = 0; point < made.sphere.size(); ++point) {
        const Parent parent = made.soil.parent[point];
        const double water = made.soil.alluvium[point] + made.soil.lake_beds[point];
        const double till = made.soil.till[point];
        CHECK(made.soil.alluvium[point] >= 0.0);
        CHECK(made.soil.lake_beds[point] >= 0.0);
        CHECK(till >= 0.0);
        // Lake beds lie only in hollows, the rivers' alluvium only outside them.
        if (made.lakes.basin[point] == Lakes::none) {
            CHECK(made.soil.lake_beds[point] == 0.0);
        } else {
            CHECK(made.soil.alluvium[point] == 0.0);
        }
        CHECK(water <= made.sediment.alluvium[point] + 1e-9);
        CHECK(till <= made.ice.till[point] + 1e-9);
        if (made.rivers.receiver[point] == point || parent == Parent::None) {
            continue;
        }
        // Soil takes the top first: till is only eaten into once what water laid is gone.
        if (till < made.ice.till[point]) {
            CHECK(water == 0.0);
        }
        const double laid = made.ice.till[point] + made.sediment.alluvium[point];
        if (water + till > 0.0) {
            CHECK_THAT(made.soil.depth[point] + water + till, WithinRel(laid, 1e-9));
        }
        if (made.soil.years[point] == made.body.soil_years) {
            CHECK(made.soil.depth[point] >= made.on_rock.depth[point]);
        }
        on_till += parent == Parent::Till ? 1 : 0;
        on_alluvium += parent == Parent::Alluvium ? 1 : 0;
        on_lake_beds += parent == Parent::LakeBed ? 1 : 0;
    }
    CHECK(on_till > 0);
    CHECK(on_alluvium > 0);
    CHECK(on_lake_beds > 0);
}

TEST_CASE("anywhere under the ground, layers lie in the order they were laid",
          "[formation][soil]") {
    const Made made{glaciated()};
    // At a point the profile is the point's own; going down, layers only ever give way to the
    // ones under them, ending in rock.
    std::uint32_t layered = 0;
    std::uint32_t chosen = 0;
    for (std::uint32_t point = 0; point < made.sphere.size(); ++point) {
        const auto spot = made.sphere.locate(made.sphere.point(point), point);
        const Profile profile = profile_at(made.sphere, made.soil, spot);
        CHECK_THAT(profile.topsoil + profile.subsoil, WithinAbs(made.soil.depth[point], 1e-9));
        CHECK_THAT(profile.organic, WithinAbs(made.soil.organic[point], 1e-9));
        CHECK_THAT(profile.till, WithinAbs(made.soil.till[point], 1e-9));
        if (made.soil.depth[point] > 0.0 && made.soil.till[point] > 1e-6) {
            ++layered;
            chosen = point;
            const double soil = profile.organic + profile.topsoil + profile.subsoil;
            CHECK(layer_at(profile, soil / 2.0) <= Layer::Subsoil);
            CHECK(layer_at(profile, soil + profile.alluvium + profile.lake_beds +
                                        (profile.till / 2.0)) == Layer::Till);
        }
    }
    REQUIRE(layered > 0);
    // Between points too, read straight from the blended thicknesses.
    const auto around = made.sphere.neighbors(chosen);
    const DVec3 between = glm::normalize(made.sphere.point(chosen) + made.sphere.point(around[0]) +
                                         made.sphere.point(around[1]));
    const Profile profile = profile_at(made.sphere, made.soil, made.sphere.locate(between, chosen));
    CHECK(profile.topsoil + profile.subsoil > 0.0);
    CHECK(profile.till > 0.0);
    const double laid = profile.organic + profile.topsoil + profile.subsoil + profile.alluvium +
                        profile.lake_beds + profile.till;
    auto previous = Layer::Organic;
    constexpr int steps = 10'000;
    for (int step = 0; step <= steps; ++step) {
        const Layer layer = layer_at(profile, (laid + 10.0) * step / steps);
        CHECK(static_cast<int>(layer) >= static_cast<int>(previous));
        previous = layer;
    }
    CHECK(previous == Layer::Rock);
}

TEST_CASE("where the coldest times' ice lay, soil is young and thin on rock", "[formation][soil]") {
    const Made made;
    int young = 0;
    int thinner = 0;
    for (std::uint32_t point = 0; point < made.sphere.size(); ++point) {
        if (made.soil.parent[point] == Parent::None) {
            continue;
        }
        const bool iced = made.ice.coldest[point] > 0.0;
        CHECK(made.soil.years[point] == (iced ? made.body.ice_left_years : made.body.soil_years));
        if (iced && made.soil.parent[point] == Parent::Rock) {
            ++young;
            CHECK(made.soil.depth[point] <= made.on_rock.depth[point]);
            thinner += made.soil.depth[point] < made.on_rock.depth[point] ? 1 : 0;
        }
    }
    REQUIRE(young > 0);
    CHECK(thinner > 0);
}

TEST_CASE("plants grow by the lesser of what warmth and rain allow", "[formation][soil]") {
    const Body body;
    CHECK(plant_growth(25.0, 2.0, body) > plant_growth(5.0, 2.0, body));
    CHECK(plant_growth(25.0, 2.0, body) > plant_growth(25.0, 0.2, body));
    CHECK(plant_growth(25.0, 0.0, body) == 0.0);
    CHECK(plant_growth(-20.0, 2.0, body) == 0.0);
    // Half a metre of rain a year holds plants back more than 10 °C does.
    CHECK_THAT(plant_growth(10.0, 0.5, body), WithinRel(3.0 * (1.0 - std::exp(-0.332)), 1e-12));
}

TEST_CASE("dead matter rots faster warm, slower dry, frozen or waterlogged", "[formation][soil]") {
    const Body body;
    CHECK_THAT(rotting_rate(20.0, 1.0, body), WithinRel(2.0 * rotting_rate(10.0, 1.0, body), 1e-9));
    CHECK(rotting_rate(10.0, 0.1, body) < rotting_rate(10.0, 1.0, body));
    CHECK(rotting_rate(-20.0, 1.0, body) == 0.0);
    CHECK(rotting_rate(10.0, 3.0, body) < 0.05 * rotting_rate(10.0, 1.0, body));
    CHECK(waterlogged(0.5) == 0.0);
    CHECK(waterlogged(3.0) == 1.0);
    CHECK(wetness(10.0, 2.0, body) > wetness(25.0, 2.0, body));
}

TEST_CASE("a store fed and rotting fills towards what it gains over how fast it rots",
          "[formation][soil]") {
    CHECK(gathered(1.0, 0.01, 0.0) == 0.0);
    CHECK_THAT(gathered(1.0, 0.01, 1.0e6), WithinRel(100.0, 1e-9));
    CHECK_THAT(gathered(1.0, 0.0, 50.0), WithinRel(50.0, 1e-12));
    const double held = gathered(1.0, 0.01, 80.0);
    const double rate = (gathered(1.0, 0.01, 81.0) - gathered(1.0, 0.01, 79.0)) / 2.0;
    CHECK_THAT(rate, WithinRel(1.0 - (0.01 * held), 1e-4));
}

TEST_CASE("plants darken the soil's top, and peat gathers where the ground is waterlogged",
          "[formation][soil]") {
    const Made made;
    int dark = 0;
    double in_air = 0.0;
    double drowned = 0.0;
    for (std::uint32_t point = 0; point < made.sphere.size(); ++point) {
        CHECK(made.soil.topsoil[point] >= 0.0);
        CHECK(made.soil.topsoil[point] <= made.soil.depth[point]);
        CHECK(made.soil.organic[point] >= 0.0);
        if (made.soil.parent[point] == Parent::None) {
            CHECK(made.soil.topsoil[point] == 0.0);
            CHECK(made.soil.organic[point] == 0.0);
            continue;
        }
        dark += made.soil.topsoil[point] > 0.0 ? 1 : 0;
        const double wet =
            wetness(made.climate.temperature[point], made.climate.rain[point], made.body);
        if (waterlogged(wet) == 0.0) {
            in_air = std::max(in_air, made.soil.organic[point]);
        } else if (waterlogged(wet) == 1.0) {
            drowned = std::max(drowned, made.soil.organic[point]);
        }
    }
    CHECK(dark > 0);
    CHECK(drowned > 10.0 * in_air);
}

TEST_CASE("soil grows down through what the wind laid first, loess over sand",
          "[formation][soil]") {
    const hog::formation::World world{Body{}, 4, 6};
    const Soil& soil = world.soil;
    const double thinnest = world.body.thinnest_drift;
    int on_wind = 0;
    for (std::uint32_t point = 0; point < world.sphere.size(); ++point) {
        if (soil.parent[point] == Parent::None) {
            continue;
        }
        const double loess =
            world.drifts.loess[point] >= thinnest ? world.drifts.loess[point] : 0.0;
        const double sand = world.drifts.sand[point] >= thinnest ? world.drifts.sand[point] : 0.0;
        const double depth = soil.depth[point];
        // The soil takes the loess first, then the sand, leaving the rest under it.
        CHECK_THAT(soil.loess[point], WithinAbs(std::max(loess - depth, 0.0), 1e-9));
        CHECK_THAT(soil.sand[point],
                   WithinAbs(std::max(sand - std::max(depth - loess, 0.0), 0.0), 1e-9));
        if (soil.parent[point] == Parent::Loess || soil.parent[point] == Parent::Sand) {
            ++on_wind;
        }
        // Down through the profile: the wind's layers lie under the soil, over what water laid.
        const Profile profile =
            profile_at(world.sphere, soil, world.sphere.locate(world.sphere.point(point), point));
        const double top = profile.organic + profile.topsoil + profile.subsoil;
        if (profile.loess > 1e-6) {
            CHECK(layer_at(profile, top + (profile.loess / 2.0)) == Layer::Loess);
        }
        if (profile.sand > 1e-6) {
            CHECK(layer_at(profile, top + profile.loess + (profile.sand / 2.0)) == Layer::Sand);
        }
    }
    CHECK(on_wind > 0);
}

TEST_CASE("rock rots in place deepest where it's warm, wet and wears slowly", "[formation][soil]") {
    const Body body;
    const double years = 1.0e6;
    using hog::formation::rotted_rock;
    CHECK(rotted_rock(25.0, 2.0, 0.0, years, body) > rotted_rock(10.0, 2.0, 0.0, years, body));
    CHECK(rotted_rock(25.0, 2.0, 0.0, years, body) > rotted_rock(25.0, 0.2, 0.0, years, body));
    CHECK(rotted_rock(25.0, 2.0, 0.0, years, body) > 10.0);
    CHECK(rotted_rock(-20.0, 2.0, 0.0, years, body) == 0.0);
    CHECK(rotted_rock(25.0, 0.0, 0.0, years, body) == 0.0);
    // Worn away as fast as it rots, none is left; it grows with the years it's had.
    CHECK(rotted_rock(25.0, 2.0, 1.0e-3, years, body) == 0.0);
    CHECK(rotted_rock(25.0, 2.0, 0.0, 2.0 * years, body) >
          1.99 * rotted_rock(25.0, 2.0, 0.0, years, body));

    // Down a profile it lies under all that was laid, over the rock.
    const Profile profile{.topsoil = 0.5, .subsoil = 1.0, .till = 2.0, .saprolite = 20.0};
    CHECK(layer_at(profile, 3.0) == Layer::Till);
    CHECK(layer_at(profile, 10.0) == Layer::Saprolite);
    CHECK(layer_at(profile, 24.0) == Layer::Rock);
}
