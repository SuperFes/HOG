#include <hog/mapgen/options.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <initializer_list>
#include <set>
#include <string_view>
#include <vector>

using hog::mapgen::FaceView;
using hog::mapgen::Layer;
using hog::mapgen::Options;
using hog::mapgen::PlanetView;
using hog::mapgen::RegionView;

namespace {

hog::Result<Options> parse(std::initializer_list<std::string_view> arguments) {
    const std::vector<std::string_view> list{arguments};
    return hog::mapgen::parse_options(list);
}

bool rejects(std::initializer_list<std::string_view> arguments) {
    const auto options = parse(arguments);
    return !options && options.error().code == hog::ErrorCode::InvalidArgument;
}

} // namespace

TEST_CASE("mapgen defaults to every layer of the whole planet", "[mapgen]") {
    const auto options = parse({});
    REQUIRE(options);
    CHECK(options->view == hog::mapgen::View{PlanetView{}});
    CHECK(options->layers.size() == hog::mapgen::layer_count);
    CHECK(options->size == 1'024);
    CHECK(options->config == "config/planet.toml");
    CHECK(options->output == "maps");
    CHECK_FALSE(options->seed);
    CHECK_FALSE(options->help);
    CHECK(options->weather == hog::mapgen::WeatherFrames{});
    CHECK(options->weather.days == 0);
    CHECK(options->wildlife == hog::mapgen::WildlifeFrames{});
    CHECK(options->seasons == hog::mapgen::SeasonFrames{});
}

TEST_CASE("mapgen reads every option", "[mapgen]") {
    const auto options = parse({"--config", "a.toml", "--seed", "18446744073709551615", "--face",
                                "4", "--layer", "height", "--size", "256", "--out", "elsewhere"});
    REQUIRE(options);
    CHECK(options->config == "a.toml");
    CHECK(options->seed == 18'446'744'073'709'551'615ULL);
    CHECK(options->view == hog::mapgen::View{FaceView{.face = 4}});
    CHECK(options->layers == std::vector{Layer::Height});
    CHECK(options->size == 256);
    CHECK(options->output == "elsewhere");

    const auto region = parse({"--region", "-41.5,172.25,30"});
    REQUIRE(region);
    CHECK(region->view ==
          hog::mapgen::View{RegionView{.latitude = -41.5, .longitude = 172.25, .width = 30'000.0}});

    const auto islands = parse({"--islands", "40"});
    REQUIRE(islands);
    CHECK(islands->view == hog::mapgen::View{hog::mapgen::IslandsView{.width = 40'000.0}});

    const auto weather =
        parse({"--weather", "3", "--every", "2", "--date", "2026-07-04", "--cell-km", "12.5"});
    REQUIRE(weather);
    using namespace std::chrono;
    CHECK(weather->weather ==
          hog::mapgen::WeatherFrames{
              .days = 3, .every = 2, .start = sys_days{2026y / July / 4}, .cell_km = 12.5});

    const auto wildlife =
        parse({"--wildlife", "5", "--every-days", "10", "--ecology", "other.toml"});
    REQUIRE(wildlife);
    CHECK(wildlife->wildlife ==
          hog::mapgen::WildlifeFrames{.years = 5, .every = 10, .ecology = "other.toml"});

    const auto seasons = parse({"--seasons", "--vegetation", "plants.toml", "--relief", "2.5"});
    REQUIRE(seasons);
    CHECK(seasons->seasons == hog::mapgen::SeasonFrames{.on = true,
                                                        .vegetation = "plants.toml",
                                                        .lighting = {.relief = 2.5}});

    const auto help = parse({"--help"});
    REQUIRE(help);
    CHECK(help->help);
}

TEST_CASE("mapgen rejects bad arguments", "[mapgen]") {
    CHECK(rejects({"--bogus"}));
    CHECK(rejects({"--face"}));
    CHECK(rejects({"--face", "6"}));
    CHECK(rejects({"--face", "-1"}));
    CHECK(rejects({"--face", "two"}));
    CHECK(rejects({"--seed", "-3"}));
    CHECK(rejects({"--seed", "12x"}));
    CHECK(rejects({"--size", "1"}));
    CHECK(rejects({"--relief", "-1"}));
    CHECK(rejects({"--relief", "steep"}));
    CHECK(rejects({"--size", "100000"}));
    CHECK(rejects({"--layer", "volcanoes"}));
    CHECK(rejects({"--region", "10,20"}));
    CHECK(rejects({"--region", "10,20,30,40"}));
    CHECK(rejects({"--region", "91,0,10"}));
    CHECK(rejects({"--region", "0,181,10"}));
    CHECK(rejects({"--region", "0,0,0"}));
    CHECK(rejects({"--region", "0,0,nan"}));
    CHECK(rejects({"--region", "0,,10"}));
    CHECK(rejects({"--islands", "0"}));
    CHECK(rejects({"--islands", "wide"}));
    CHECK(rejects({"--weather", "0"}));
    CHECK(rejects({"--weather", "400"}));
    CHECK(rejects({"--every", "0"}));
    CHECK(rejects({"--wildlife", "0"}));
    CHECK(rejects({"--wildlife", "101"}));
    CHECK(rejects({"--every-days", "0"}));
    CHECK(rejects({"--weather", "1", "--wildlife", "1"}));
    CHECK(rejects({"--seasons", "--weather", "1"}));
    CHECK(rejects({"--wildlife", "1", "--seasons"}));
    CHECK(rejects({"--date", "2026-02-30"}));
    CHECK(rejects({"--date", "2026-2-3"}));
    CHECK(rejects({"--date", "2026/02/03"}));
    CHECK(rejects({"--cell-km", "0"}));
    CHECK(rejects({"--cell-km", "inf"}));
}

TEST_CASE("every mapgen layer has a distinct name", "[mapgen]") {
    std::set<std::string_view> names;
    for (int layer = 0; layer < hog::mapgen::layer_count; ++layer) {
        names.insert(to_string(static_cast<Layer>(layer)));
    }
    CHECK(names.size() == hog::mapgen::layer_count);
    CHECK_FALSE(names.contains("unknown"));
}

TEST_CASE("mapgen's usage names every layer", "[mapgen]") {
    for (int layer = 0; layer < hog::mapgen::layer_count; ++layer) {
        CHECK(hog::mapgen::usage().contains(to_string(static_cast<Layer>(layer))));
    }
}
