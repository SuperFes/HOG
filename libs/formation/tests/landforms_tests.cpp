#include <hog/formation/landforms.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <ranges>

using hog::formation::Bedrock;
using hog::formation::bedrock_on;
using hog::formation::Body;
using hog::formation::Climate;
using hog::formation::climate_on;
using hog::formation::Crust;
using hog::formation::crust_on;
using hog::formation::Ice;
using hog::formation::ice_on;
using hog::formation::ice_shaped;
using hog::formation::Landforms;
using hog::formation::landforms_on;
using hog::formation::Meeting;
using hog::formation::Outcrop;
using hog::formation::outcrop_on;
using hog::formation::Plates;
using hog::formation::Rivers;
using hog::formation::rivers_on;
using hog::formation::Rock;
using hog::formation::rock_strength;
using hog::formation::Sphere;

TEST_CASE("crystalline rock stands steepest, soft flat beds least", "[formation][landforms]") {
    CHECK(rock_strength(Rock::Basement, 0.0, 0.0) > rock_strength(Rock::Lava, 0.0, 0.0));
    CHECK(rock_strength(Rock::Lava, 0.0, 0.0) > rock_strength(Rock::Folded, 0.0, 0.0));
    CHECK(rock_strength(Rock::Folded, 0.0, 0.0) > rock_strength(Rock::Layered, 0.0, 0.0));
    // Hard beds hold cliffs in layered rock.
    CHECK(rock_strength(Rock::Layered, 1.0, 0.0) > rock_strength(Rock::Layered, 0.0, 0.0));
}

namespace {

struct Made {
    Body body;
    Sphere layout{5, body.seed};
    Sphere sphere{6, body.seed + 1};
    Plates plates{body, layout};
    Crust crust = crust_on(sphere, plates, body);
    Climate climate = climate_on(sphere, crust.base, crust.sea, body);
    Rivers worn = rivers_on(sphere, crust, climate, body);
    Ice ice = ice_on(sphere, crust, climate, worn, body);
    Rivers rivers = ice_shaped(sphere, worn, ice);
    Bedrock bedrock = bedrock_on(sphere, crust, body);
    Outcrop outcrop = outcrop_on(sphere, crust, bedrock, rivers, body);
    Landforms forms = landforms_on(sphere, crust, bedrock, outcrop, climate, rivers, ice, body);
};

} // namespace

TEST_CASE("landforms are likelihoods on land only", "[formation][landforms]") {
    const Made made;
    const Landforms& forms = made.forms;
    for (std::uint32_t point = 0; point < made.sphere.size(); ++point) {
        const bool sea = made.rivers.receiver[point] == point;
        for (const auto* field : {&forms.canyons, &forms.mesas, &forms.escarpments, &forms.badlands,
                                  &forms.karst, &forms.sea_cliffs, &forms.glacial,
                                  &forms.fault_scarps, &forms.volcanoes, &forms.scree}) {
            CHECK((*field)[point] >= 0.0);
            CHECK((*field)[point] <= 1.0);
            if (sea) {
                CHECK((*field)[point] == 0.0);
            }
        }
    }
}

TEST_CASE("each landform comes only where what makes it is", "[formation][landforms]") {
    const Made made;
    const Landforms& forms = made.forms;
    for (std::uint32_t point = 0; point < made.sphere.size(); ++point) {
        const Rock rock = made.outcrop.rock[point];
        if (rock != Rock::Layered && rock != Rock::Folded) {
            CHECK(forms.karst[point] == 0.0);
        }
        if (made.bedrock.soluble[point] == 0.0) {
            CHECK(forms.karst[point] == 0.0);
        }
        if (made.ice.carved[point] <= 20.0) {
            CHECK(forms.glacial[point] == 0.0);
        }
        if (rock != Rock::Layered && rock != Rock::Lava) {
            CHECK(forms.mesas[point] == 0.0);
        }
        const Meeting meeting = made.crust.meeting[point];
        if (meeting == Meeting::None || meeting == Meeting::Collision ||
            meeting == Meeting::Transform) {
            CHECK(forms.volcanoes[point] == 0.0);
        }
        if (made.ice.thickness[point] > 0.0) {
            // Ice hides everything but what it carved itself.
            for (const auto* field :
                 {&forms.canyons, &forms.mesas, &forms.escarpments, &forms.badlands, &forms.karst,
                  &forms.sea_cliffs, &forms.fault_scarps, &forms.volcanoes, &forms.scree}) {
                CHECK((*field)[point] == 0.0);
            }
        }
        const auto sea = [&](std::uint32_t next) { return made.rivers.receiver[next] == next; };
        if (!std::ranges::any_of(made.sphere.neighbors(point), sea)) {
            CHECK(forms.sea_cliffs[point] == 0.0);
        }
    }
    // The default planet has some of each.
    for (const auto* field :
         {&forms.canyons, &forms.mesas, &forms.escarpments, &forms.karst, &forms.sea_cliffs,
          &forms.glacial, &forms.fault_scarps, &forms.volcanoes, &forms.scree}) {
        CHECK(std::ranges::max(*field) > 0.3);
    }
}

TEST_CASE("a wetter world rounds off its mesas", "[formation][landforms]") {
    const Made made;
    Body wet = made.body;
    wet.lake_loss *= 0.2; // the air takes far less, so everywhere is wetter against it
    const Landforms wetter = landforms_on(made.sphere, made.crust, made.bedrock, made.outcrop,
                                          made.climate, made.rivers, made.ice, wet);
    double before = 0.0;
    double after = 0.0;
    for (std::uint32_t point = 0; point < made.sphere.size(); ++point) {
        before += made.forms.mesas[point];
        after += wetter.mesas[point];
    }
    CHECK(after < before);
}
