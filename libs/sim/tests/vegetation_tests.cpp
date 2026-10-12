#include <hog/sim/vegetation.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <string>
#include <string_view>

using hog::planet::Biome;
using hog::sim::parse_vegetation;

namespace {

std::size_t at(Biome biome) {
    return static_cast<std::size_t>(biome);
}

std::string error_of(std::string_view text) {
    const auto parsed = parse_vegetation(text);
    return parsed ? std::string{} : parsed.error().message;
}

} // namespace

TEST_CASE("vegetation reads growth, dryness and fire", "[sim][vegetation]") {
    const auto vegetation = parse_vegetation(R"(
[growth]
capacity = { temperate_forest = 20, "boreal forest" = 10 }
years = { temperate_forest = 60 }
seeds = 0.05
warm = 12

[dryness]
demand = 0.2

[fire]
damp = 0.3
spread = 4
burn_days = 3
)");
    REQUIRE(vegetation);
    CHECK(vegetation->growth.capacity[at(Biome::TemperateForest)] == 20.0);
    CHECK(vegetation->growth.capacity[at(Biome::BorealForest)] == 10.0);
    CHECK(vegetation->growth.capacity[at(Biome::Grassland)] == 0.0);
    CHECK(vegetation->growth.years[at(Biome::TemperateForest)] == 60.0);
    CHECK(vegetation->growth.years[at(Biome::Grassland)] == 30.0);
    CHECK(vegetation->growth.seeds == 0.05);
    CHECK(vegetation->growth.cold == 0.0);
    CHECK(vegetation->growth.warm == 12.0);
    CHECK(vegetation->dryness.demand == 0.2);
    CHECK(vegetation->dryness.climate == 0.5);
    CHECK(vegetation->fire.damp == 0.3);
    CHECK(vegetation->fire.parched == 0.9);
    CHECK(vegetation->fire.spread == 4.0);
    CHECK(vegetation->fire.burn_days == 3.0);

    const auto empty = parse_vegetation("");
    REQUIRE(empty);
    CHECK(empty->fire.lightning == hog::sim::FireConfig{}.lightning);
}

TEST_CASE("vegetation reports what it can't read", "[sim][vegetation]") {
    using Catch::Matchers::ContainsSubstring;
    CHECK_THAT(error_of("[growth]\ncapacity = { swamp = 3 }"),
               ContainsSubstring("growth.capacity"));
    CHECK_THAT(error_of("[growth]\nyears = { grassland = 0 }"),
               ContainsSubstring("growth.years.grassland"));
    CHECK_THAT(error_of("[growth]\ncold = 20"), ContainsSubstring("growth.warm"));
    CHECK_THAT(error_of("[growth]\nseeds = 0"), ContainsSubstring("growth.seeds"));
    CHECK_THAT(error_of("[dryness]\nclimate = 2"), ContainsSubstring("dryness.climate"));
    CHECK_THAT(error_of("[fire]\ndamp = 0.9"), ContainsSubstring("fire.parched"));
    CHECK_THAT(error_of("[fire]\nburn_days = 0"), ContainsSubstring("fire.burn_days"));
    CHECK_THAT(error_of("[fire]\nsmoke = 1"), ContainsSubstring("fire.smoke"));
    CHECK_THAT(error_of("[weather]"), ContainsSubstring("weather"));
    CHECK(parse_vegetation("[fire").error().code == hog::ErrorCode::Parse);
}

TEST_CASE("vegetation loads from a file", "[sim][vegetation]") {
    const auto development = hog::sim::load_vegetation(HOG_SOURCE_DIR "/config/forest.toml");
    REQUIRE(development);
    CHECK(development->growth.capacity[at(Biome::TemperateForest)] > 0.0);
    CHECK(development->growth.capacity[at(Biome::Ocean)] == 0.0);
    CHECK(hog::sim::load_vegetation("no/such/forest.toml").error().code ==
          hog::ErrorCode::NotFound);
}
