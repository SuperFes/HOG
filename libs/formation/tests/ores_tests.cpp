#include <hog/formation/ice.hpp>
#include <hog/formation/ores.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>

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
using hog::formation::Lakes;
using hog::formation::lakes_on;
using hog::formation::Meeting;
using hog::formation::Ores;
using hog::formation::ores_on;
using hog::formation::Outcrop;
using hog::formation::outcrop_on;
using hog::formation::Plates;
using hog::formation::Rivers;
using hog::formation::rivers_on;
using hog::formation::Sediment;
using hog::formation::sediment_on;
using hog::formation::Soil;
using hog::formation::soil_on;
using hog::formation::Sphere;

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
    Lakes lakes = lakes_on(sphere, rivers, climate, body);
    Sediment sediment = sediment_on(sphere, rivers, lakes, ice.outwash, body);
    Soil soil = soil_on(sphere, rivers, climate, ice, lakes, sediment, body);
    Bedrock bedrock = bedrock_on(sphere, crust, body);
    Outcrop outcrop = outcrop_on(sphere, crust, bedrock, rivers, body);

    [[nodiscard]] Ores ores_with(const Body& with) const {
        return ores_on(sphere, crust, bedrock, outcrop, climate, rivers, lakes, sediment, soil,
                       with);
    }
};

std::vector<const std::vector<double>*> fields(const Ores& ores) {
    return {&ores.copper,   &ores.gold,     &ores.placers,   &ores.banded_iron,
            &ores.laterite, &ores.bog_iron, &ores.lead_zinc, &ores.salt,
            &ores.coal,     &ores.oil,      &ores.diamonds};
}

} // namespace

TEST_CASE("ores are likelihoods on land, each made where its process was", "[formation][ores]") {
    const Made made;
    const Ores ores = made.ores_with(made.body);
    for (std::uint32_t point = 0; point < made.sphere.size(); ++point) {
        const bool sea = made.rivers.receiver[point] == point;
        for (const auto* field : fields(ores)) {
            CHECK((*field)[point] >= 0.0);
            CHECK((*field)[point] <= 1.0);
            if (sea) {
                CHECK((*field)[point] == 0.0);
            }
        }
        if (sea) {
            continue;
        }
        const Meeting meeting = made.crust.meeting[point];
        if (meeting != Meeting::Subduction && meeting != Meeting::IslandArc) {
            CHECK(ores.copper[point] == 0.0);
        }
        if (made.soil.organic[point] <= 0.1) {
            CHECK(ores.bog_iron[point] == 0.0);
        }
        if (made.lakes.salt_flat[point] != 0) {
            CHECK(ores.salt[point] == 1.0);
        }
        if (made.sediment.alluvium[point] <= 0.0) {
            CHECK(ores.placers[point] == 0.0);
        }
        if (made.soil.years[point] < made.body.soil_years ||
            made.climate.temperature[point] < 18.0) {
            CHECK(ores.laterite[point] == 0.0);
        }
    }
    for (const auto* field : fields(ores)) {
        CHECK(std::ranges::max(*field) > 0.0);
    }
}

TEST_CASE("an ore made scarcer is found less, and none at all made absent", "[formation][ores]") {
    const Made made;
    const Ores usual = made.ores_with(made.body);
    Body poor = made.body;
    poor.ore_abundance.copper = 0.0;
    poor.ore_abundance.coal = 0.5;
    const Ores scarcer = made.ores_with(poor);
    CHECK(std::ranges::max(scarcer.copper) == 0.0);
    double coal = 0.0;
    double less = 0.0;
    for (std::uint32_t point = 0; point < made.sphere.size(); ++point) {
        coal += usual.coal[point];
        less += scarcer.coal[point];
        CHECK(scarcer.gold[point] == usual.gold[point]);
    }
    CHECK(less < coal);
}
