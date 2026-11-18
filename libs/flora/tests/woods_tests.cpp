#include <hog/flora/woods.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <numeric>

using Catch::Approx;
using hog::flora::Habitat;
using hog::flora::Kind;
using hog::flora::Woods;
using hog::flora::woods_of;

namespace {

// Deep soil on gentle ground, high over its drainage, unless said otherwise.
Habitat climate(double temperature, double swing, double rain) {
    return Habitat{.temperature = temperature,
                   .swing = swing,
                   .rain = rain,
                   .over_drainage = 30.0,
                   .soil = 1.0,
                   .bare = 0.0,
                   .slope = 0.1,
                   .ice = 0.0,
                   .water = 0.0,
                   .sea = false};
}

Kind most(const Woods& woods) {
    return static_cast<Kind>(std::ranges::max_element(woods.shares) - woods.shares.begin());
}

double total(const Woods& woods) {
    return std::accumulate(woods.shares.begin(), woods.shares.end(), 0.0);
}

} // namespace

TEST_CASE("the year's warmth as plants feel it", "[flora][woods]") {
    // Never freezing, never over 30: biotemperature is the mean, and it grows all year.
    const auto mild = hog::flora::warmth_of(climate(15.0, 5.0, 1.0));
    CHECK(mild.biotemperature == Approx(15.0));
    CHECK(mild.growing == 1.0);
    CHECK(mild.growing_mean == Approx(15.0));
    CHECK(mild.coldest == 10.0);
    // Below freezing half the year: the cold months count as nothing.
    const auto cold = hog::flora::warmth_of(climate(0.0, 10.0, 1.0));
    CHECK(cold.biotemperature > 0.0);
    CHECK(cold.biotemperature < 5.0);
    CHECK(cold.growing < 0.5);
    CHECK(cold.growing_mean > 5.0);
    // Rain over what the warmth takes, or the groundwater the roots reach.
    CHECK(hog::flora::moisture_of(climate(15.0, 0.0, 0.8839), mild) == Approx(1.0).epsilon(1e-3));
    Habitat by_water = climate(15.0, 0.0, 0.0);
    by_water.over_drainage = 1.0;
    by_water.flow = 10.0;
    CHECK(hog::flora::moisture_of(by_water, hog::flora::warmth_of(by_water)) == 2.0);
    // A dry wash's floor gives none, nor a desert stream's.
    by_water.flow = 0.2;
    CHECK(hog::flora::moisture_of(by_water, hog::flora::warmth_of(by_water)) == 0.0);
    by_water.flow = 0.0;
    CHECK(hog::flora::moisture_of(by_water, hog::flora::warmth_of(by_water)) == 0.0);
}

TEST_CASE("each climate grows its own woods", "[flora][woods]") {
    const Woods tropical = woods_of(climate(26.0, 1.0, 3.0));
    CHECK(tropical.cover > 0.8);
    CHECK(most(tropical) == Kind::Evergreen);

    const Woods boreal = woods_of(climate(-2.0, 18.0, 0.5));
    CHECK(boreal.cover > 0.5);
    CHECK(most(boreal) == Kind::Needleleaf);

    const Woods temperate = woods_of(climate(10.0, 10.0, 0.9));
    CHECK(temperate.cover > 0.8);
    CHECK(most(temperate) == Kind::Deciduous);

    const Woods savanna = woods_of(climate(24.0, 4.0, 0.6));
    CHECK(savanna.cover > 0.1);
    CHECK(savanna.cover < 0.6);
    CHECK(most(savanna) == Kind::Dry);

    CHECK(woods_of(climate(22.0, 8.0, 0.1)).cover < 0.02);
    // Over the treeline, none.
    CHECK(woods_of(climate(-8.0, 8.0, 1.0)).cover == 0.0);

    for (const Woods& woods : {tropical, boreal, temperate, savanna}) {
        CHECK(total(woods) == Approx(1.0));
    }
}

TEST_CASE("water near the ground grows trees in the dry and wetland ones in the wet",
          "[flora][woods]") {
    Habitat oasis = climate(22.0, 8.0, 0.1);
    oasis.over_drainage = 1.0;
    oasis.flow = 10.0;
    const Woods watered = woods_of(oasis);
    CHECK(watered.cover > 0.3);
    CHECK(watered.share(Kind::Dry) < 0.1);
    oasis.flow = 0.0;
    CHECK(woods_of(oasis).cover < 0.02);

    Habitat fen = climate(8.0, 10.0, 0.8);
    fen.over_drainage = 0.2;
    fen.flow = 1.0;
    CHECK(most(woods_of(fen)) == Kind::Wetland);

    Habitat swamp = climate(18.0, 6.0, 1.2);
    swamp.water = 0.4;
    const Woods standing = woods_of(swamp);
    CHECK(standing.cover > 0.5);
    CHECK(standing.share(Kind::Wetland) == 1.0);
    swamp.water = 5.0;
    CHECK(woods_of(swamp).cover == 0.0);

    // Mangroves stand in warm shallow sea, never one that nears frost.
    Habitat shore = climate(26.0, 3.0, 2.0);
    shore.water = 0.5;
    shore.sea = true;
    const Woods mangrove = woods_of(shore);
    CHECK(mangrove.cover > 0.5);
    CHECK(mangrove.share(Kind::Wetland) == 1.0);
    shore.temperature = 12.0;
    CHECK(woods_of(shore).cover == 0.0);
}

TEST_CASE("bare rock, steep ground and ice hold few trees or none", "[flora][woods]") {
    Habitat rock = climate(10.0, 10.0, 0.9);
    rock.soil = 0.0;
    rock.bare = 1.0;
    const Woods cracks = woods_of(rock);
    CHECK(cracks.cover > 0.0);
    CHECK(cracks.cover < 0.05);
    Habitat cliff = climate(10.0, 10.0, 0.9);
    cliff.slope = 3.0;
    CHECK(woods_of(cliff).cover == 0.0);
    Habitat glacier = climate(10.0, 10.0, 0.9);
    glacier.ice = 5.0;
    CHECK(woods_of(glacier).cover == 0.0);
}

// A step would jump however finely it is crossed; fading, a fine crossing changes little.
TEST_CASE("woods fade from one climate to the next, never stepping", "[flora][woods]") {
    const auto close = [](const Woods& one, const Woods& other) {
        CHECK(std::abs(one.cover - other.cover) < 0.02);
        if (one.cover > 0.05 && other.cover > 0.05) {
            for (std::size_t kind = 0; kind < hog::flora::kinds; ++kind) {
                CAPTURE(kind);
                CHECK(std::abs(one.shares.at(kind) - other.shares.at(kind)) < 0.05);
            }
        }
    };
    for (const double rain : {0.2, 0.6, 1.5}) {
        CAPTURE(rain);
        Woods last = woods_of(climate(-15.0, 8.0, rain));
        for (int step = 0; step <= 4'700; ++step) {
            const double temperature = -15.0 + (0.01 * step);
            CAPTURE(temperature);
            const Woods here = woods_of(climate(temperature, 8.0, rain));
            close(last, here);
            last = here;
        }
    }
    for (const double temperature : {2.0, 12.0, 24.0}) {
        CAPTURE(temperature);
        Woods last = woods_of(climate(temperature, 6.0, 0.0));
        for (int step = 0; step <= 3'000; ++step) {
            const double rain = 0.001 * step;
            CAPTURE(rain);
            const Woods here = woods_of(climate(temperature, 6.0, rain));
            close(last, here);
            last = here;
        }
    }
    Habitat over = climate(15.0, 6.0, 0.3);
    over.flow = 1.0;
    Woods last = woods_of(over);
    for (int step = 6'000; step >= 0; --step) {
        const double height = 0.005 * step;
        CAPTURE(height);
        over.over_drainage = height;
        const Woods here = woods_of(over);
        close(last, here);
        last = here;
    }
}

TEST_CASE("stands lean off the habitat into groves, glades and the odd outlier", "[flora][woods]") {
    const Habitat temperate = climate(10.0, 10.0, 0.9);
    const Woods even = woods_of(temperate);
    REQUIRE(most(even) == Kind::Deciduous);
    REQUIRE(even.share(Kind::Needleleaf) > 0.0);

    // A wander gathers a kind already there into a grove of its own, the rest still mixed in.
    hog::flora::Lean conifers;
    conifers.wander.at(static_cast<std::size_t>(Kind::Needleleaf)) = 1.0;
    conifers.wander.at(static_cast<std::size_t>(Kind::Deciduous)) = -1.0;
    const Woods grove = woods_of(temperate, conifers);
    CHECK(grove.cover == even.cover);
    CHECK(grove.share(Kind::Needleleaf) > even.share(Kind::Needleleaf));
    CHECK(grove.share(Kind::Deciduous) > 0.05);
    CHECK(total(grove) == Approx(1.0));

    // An outlier stands where the habitat suits it a little, never where it can't live: oaks in
    // the conifers' cool margin, never palms.
    const Habitat boreal = climate(1.0, 16.0, 0.6);
    REQUIRE(woods_of(boreal).share(Kind::Deciduous) < 0.05);
    for (const Kind kind : {Kind::Deciduous, Kind::Palm, Kind::Evergreen}) {
        CAPTURE(kind);
        hog::flora::Lean odd;
        odd.outlier.at(static_cast<std::size_t>(kind)) = 1.0;
        const Woods outlying = woods_of(boreal, odd);
        if (kind == Kind::Deciduous) {
            CHECK(most(outlying) == Kind::Deciduous);
        } else {
            CHECK(outlying.share(kind) == 0.0);
        }
        CHECK(outlying.cover == woods_of(boreal).cover);
    }

    hog::flora::Lean glade;
    glade.glade = 1.0;
    CHECK(woods_of(temperate, glade).cover < 0.5 * even.cover);
    CHECK(woods_of(temperate, glade).shares == even.shares);
}

TEST_CASE("stands fade as their lean wanders, never stepping", "[flora][woods]") {
    const Habitat temperate = climate(12.0, 8.0, 1.0);
    for (std::size_t kind = 0; kind < hog::flora::kinds; ++kind) {
        CAPTURE(kind);
        hog::flora::Lean lean;
        lean.wander.at(kind) = -1.0;
        Woods last = woods_of(temperate, lean);
        for (int step = -100; step <= 100; ++step) {
            lean.wander.at(kind) = 0.01 * step;
            lean.outlier.at(kind) = 0.005 * (step + 100);
            lean.glade = 0.005 * (step + 100);
            const Woods here = woods_of(temperate, lean);
            CHECK(std::abs(here.cover - last.cover) < 0.02);
            for (std::size_t other = 0; other < hog::flora::kinds; ++other) {
                CHECK(std::abs(here.shares.at(other) - last.shares.at(other)) < 0.05);
            }
            last = here;
        }
    }
}
