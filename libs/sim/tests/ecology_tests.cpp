#include <hog/sim/ecology.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <chrono>
#include <string>

using hog::planet::Biome;
using hog::sim::Food;
using hog::sim::parse_ecology;

namespace {

std::size_t at(Biome biome) {
    return static_cast<std::size_t>(biome);
}

std::string error_of(std::string_view text) {
    const auto parsed = parse_ecology(text);
    return parsed ? std::string{} : parsed.error().message;
}

} // namespace

TEST_CASE("an ecology reads forage and species", "[sim][ecology]") {
    const auto ecology = parse_ecology(R"(
[forage]
growth_days = 20
cold = 2
rot_days = 5
capacity = { grassland = 300000, "temperate forest" = 200000, boreal_forest = 1e5 }

[[species]]
name = "deer"
mass = 80
diet = { forage = 1.0 }
habitat = { grassland = 1.0, temperate_forest = 0.5 }
breeding = { start_day = 40, days = 50 }
lifespan_years = 12
hungry_below = 0.6
comfort = [-25, 35]
dormant_below = -10
cycle = { years = 13, boom = 4, width_years = 2 }
recovery = { years = [2, 6], founders = 12, after_extinction = "successor", extinct_years = [5, 9], successor = "wolf" }
events = { baseline_years = 5, collapse = 0.1, overpopulation = 4, least = 20 }

[[species]]
name = "wolf"
moves = "land"
diet = { deer = 1.0, carrion = 0.25 }
recovery = { after_extinction = "relict" }
)");
    REQUIRE(ecology);
    CHECK(ecology->forage.growth == std::chrono::days{20});
    CHECK(ecology->forage.cold == 2.0);
    CHECK(ecology->forage.warm == 15.0);
    CHECK(ecology->forage.capacity[at(Biome::Grassland)] == 300'000.0);
    CHECK(ecology->forage.capacity[at(Biome::TemperateForest)] == 200'000.0);
    CHECK(ecology->forage.capacity[at(Biome::BorealForest)] == 100'000.0);
    CHECK(ecology->forage.capacity[at(Biome::Desert)] == 0.0);

    REQUIRE(ecology->species.size() == 2);
    const auto& deer = ecology->species[0];
    CHECK(deer.mass == 80.0);
    CHECK(deer.habitat[at(Biome::TemperateForest)] == 0.5);
    CHECK(deer.breeding.start == 40.0);
    CHECK(deer.breeding.days == 50.0);
    CHECK(deer.lifespan == 12.0);
    CHECK(deer.hungry_below == 0.6);
    CHECK(deer.cold == -25.0);
    CHECK(deer.heat == 35.0);
    CHECK(deer.dormant_below == -10.0);
    CHECK(deer.cycle.years == 13.0);
    CHECK(deer.cycle.width == 2.0);
    CHECK(deer.recovery.earliest == 2.0);
    CHECK(deer.recovery.latest == 6.0);
    CHECK(deer.recovery.founders == 12.0);
    CHECK(deer.recovery.after_extinction == hog::sim::Recovery::After::Successor);
    CHECK(deer.recovery.extinct_earliest == 5.0);
    CHECK(deer.recovery.extinct_latest == 9.0);
    CHECK(deer.recovery.successor == 1U);
    CHECK(deer.events.baseline_years == 5.0);
    CHECK(deer.events.collapse == 0.1);
    CHECK(deer.events.overpopulation == 4.0);
    CHECK(deer.events.least == 20.0);
    REQUIRE(deer.diet.size() == 1);
    CHECK(deer.diet[0].kind == Food::Kind::Forage);

    const auto& wolf = ecology->species[1];
    CHECK_FALSE(wolf.dormant_below);
    CHECK(wolf.recovery.after_extinction == hog::sim::Recovery::After::Relict);
    CHECK(wolf.recovery.latest == 10.0);
    CHECK(wolf.events.collapse == 0.2);
    REQUIRE(wolf.diet.size() == 2);
    const auto prey = std::ranges::find(wolf.diet, Food::Kind::Prey, &Food::kind);
    REQUIRE(prey != wolf.diet.end());
    CHECK(prey->prey == 0);
    CHECK(ecology->find("wolf") == 1U);
    CHECK_FALSE(ecology->find("bear"));
}

TEST_CASE("an empty ecology has defaults and no species", "[sim][ecology]") {
    const auto ecology = parse_ecology("");
    REQUIRE(ecology);
    CHECK(ecology->species.empty());
    CHECK(hog::sim::Species{}.recovery.after_extinction == hog::sim::Recovery::After::Admin);
    CHECK(ecology->forage.rot == std::chrono::days{10});
}

TEST_CASE("an ecology rejects what it can't use", "[sim][ecology]") {
    using Catch::Matchers::ContainsSubstring;
    CHECK_THAT(error_of("[forage]\nsprout = 1"), ContainsSubstring("forage.sprout"));
    CHECK_THAT(error_of("[forage]\ncapacity = { swamp = 1 }"), ContainsSubstring("swamp"));
    CHECK_THAT(error_of("[forage]\ncold = 20\nwarm = 10"), ContainsSubstring("forage.warm"));
    CHECK_THAT(error_of("[forage]\ngrowth_days = nan"), ContainsSubstring("growth_days"));
    CHECK_THAT(error_of("[[species]]\nmass = 3"), ContainsSubstring("species[0].name"));
    CHECK_THAT(error_of("[[species]]\nname = \"a\"\nmass = 0"), ContainsSubstring("mass"));
    CHECK_THAT(error_of("[[species]]\nname = \"a\"\nmoves = \"burrow\""),
               ContainsSubstring("moves"));
    CHECK_THAT(error_of("[[species]]\nname = \"a\"\ncomfort = [10, 0]"),
               ContainsSubstring("comfort"));
    CHECK_THAT(error_of("[[species]]\nname = \"a\"\ndiet = { grass = 1 }"),
               ContainsSubstring("grass"));
    CHECK_THAT(error_of("[[species]]\nname = \"a\"\nhabitat = { grassland = 2 }"),
               ContainsSubstring("grassland"));
    CHECK_THAT(error_of("[[species]]\nname = \"a\"\ncycle = { years = 2, width_years = 3 }"),
               ContainsSubstring("width_years"));
    CHECK_THAT(error_of("[[species]]\nname = \"a\"\nrecovery = { years = [5, 2] }"),
               ContainsSubstring("recovery.years"));
    CHECK_THAT(error_of("[[species]]\nname = \"a\"\nrecovery = { after_extinction = \"pray\" }"),
               ContainsSubstring("after_extinction"));
    CHECK_THAT(error_of("[[species]]\nname = \"a\"\nrecovery = { after_extinction = "
                        "\"successor\" }"),
               ContainsSubstring("needs a successor"));
    CHECK_THAT(error_of("[[species]]\nname = \"a\"\nrecovery = { after_extinction = "
                        "\"successor\", successor = \"a\" }"),
               ContainsSubstring("another species"));
    CHECK_THAT(error_of("[[species]]\nname = \"a\"\nrecovery = { successor = \"b\" }\n"
                        "[[species]]\nname = \"b\""),
               ContainsSubstring("isn't succeeded"));
    CHECK_THAT(error_of("[[species]]\nname = \"a\"\nevents = { collapse = 1.5 }"),
               ContainsSubstring("events.collapse"));
    CHECK_THAT(error_of("[[species]]\nname = \"a\"\nevents = { overpopulation = 0.5 }"),
               ContainsSubstring("events.overpopulation"));
    CHECK_THAT(error_of("[[species]]\nname = \"a\"\nevents = { boom = 1 }"),
               ContainsSubstring("events.boom"));
    CHECK_THAT(error_of("[[species]]\nname = \"a\"\n[[species]]\nname = \"a\""),
               ContainsSubstring("twice"));
    CHECK_THAT(error_of("[[species]]\nname = \"carrion\""), ContainsSubstring("carrion"));
    CHECK_THAT(error_of("species = 3"), ContainsSubstring("[[species]]"));
    CHECK_THAT(error_of("[weather]"), ContainsSubstring("weather"));
    CHECK(parse_ecology("[forage").error().code == hog::ErrorCode::Parse);
}

TEST_CASE("an ecology loads from a file", "[sim][ecology]") {
    const auto development = hog::sim::load_ecology(HOG_SOURCE_DIR "/config/ecology.toml");
    REQUIRE(development);
    CHECK(development->forage.capacity[at(Biome::Grassland)] > 0.0);
    CHECK(hog::sim::load_ecology("no/such/ecology.toml").error().code == hog::ErrorCode::NotFound);
}
