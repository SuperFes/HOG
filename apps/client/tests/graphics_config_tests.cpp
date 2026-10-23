#include <hog/client/graphics_config.hpp>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <optional>

using hog::client::find_graphics_config;
using hog::client::GraphicsConfig;
using hog::client::parse_graphics_config;

TEST_CASE("graphics config left empty keeps the defaults", "[client][graphics]") {
    const auto config = parse_graphics_config("");
    REQUIRE(config);
    CHECK(*config == GraphicsConfig{});
}

TEST_CASE("the example graphics config is the defaults", "[client][graphics]") {
    const auto config = hog::client::load_graphics_config(std::filesystem::path{HOG_SOURCE_DIR} /
                                                          "config/graphics.example.toml");
    REQUIRE(config);
    CHECK(*config == GraphicsConfig{});
}

TEST_CASE("graphics config sets how far and thick the grass grows", "[client][graphics]") {
    const auto config = parse_graphics_config(R"(
[grass]
density = 20.0
hold = 15
far = 180.5
finest = 0.5
fine = 35
coarsest = 4.0

[bushes]
far = 120

[forest]
full = 40
mid = 90
far = 160

[trees]
far = 600
)");
    REQUIRE(config);
    const hog::client::GrassBudget& grass = config->plants;
    CHECK(grass.density == 20.0);
    CHECK(grass.hold == 15.0);
    CHECK(grass.reach == 180.5);
    CHECK(grass.finest == 0.5);
    CHECK(grass.fine == 35.0);
    CHECK(grass.coarsest == 4.0);
    CHECK(grass.grass().reach == 180.5);
    CHECK(grass.bush_far == 120.0);
    CHECK(grass.forest_full == 40.0);
    CHECK(grass.forest_mid == 90.0);
    CHECK(grass.forest_far == 160.0);
    CHECK(grass.tree_far == 600.0);
    // What it doesn't set keeps its default.
    CHECK(grass.tree_coarsest == hog::client::GrassBudget{}.tree_coarsest);
}

TEST_CASE("graphics config rejects what it can't draw", "[client][graphics]") {
    CHECK_FALSE(parse_graphics_config("[grass"));
    CHECK_FALSE(parse_graphics_config("[sky]\n"));
    CHECK_FALSE(parse_graphics_config("grass = 1\n"));
    CHECK_FALSE(parse_graphics_config("[grass]\nlawn = 30\n"));
    CHECK_FALSE(parse_graphics_config("[bushes]\nnear = 30\n"));
    CHECK_FALSE(parse_graphics_config("[grass]\nfull = 30\n"));
    CHECK_FALSE(parse_graphics_config("[grass]\ndensity = -1\n"));
    CHECK_FALSE(parse_graphics_config("[grass]\ndensity = 'thick'\n"));
    CHECK_FALSE(parse_graphics_config("[grass]\nfinest = 0\n"));
    CHECK_FALSE(parse_graphics_config("[grass]\nfinest = 8\n"));
    CHECK_FALSE(parse_graphics_config("[grass]\ncoarsest = 20\n"));
    CHECK_FALSE(parse_graphics_config("[grass]\nhold = 0\n"));
    CHECK_FALSE(parse_graphics_config("[grass]\nfine = 0\n"));
    CHECK_FALSE(parse_graphics_config("[grass]\nfine = 300\n"));
    CHECK_FALSE(parse_graphics_config("[grass]\nfar = 5000\n"));
    CHECK_FALSE(parse_graphics_config("[bushes]\nfar = 20000\n"));
    CHECK_FALSE(parse_graphics_config("[forest]\nfull = 0\n"));
    CHECK_FALSE(parse_graphics_config("[forest]\nfull = 200\n"));
    CHECK_FALSE(parse_graphics_config("[forest]\nmid = 300\n"));
    CHECK_FALSE(parse_graphics_config("[trees]\nfar = 200\n"));
    CHECK_FALSE(parse_graphics_config("[trees]\nfar = 20000\n"));
}

TEST_CASE("a machine without a graphics config draws the defaults", "[client][graphics]") {
    // Run from out of the source tree, so neither it nor anything above it keeps one.
    const std::filesystem::path nowhere = std::filesystem::temp_directory_path() / "hog-nowhere";
    const std::filesystem::path was = std::filesystem::current_path();
    std::filesystem::current_path(std::filesystem::temp_directory_path());
    const auto config = find_graphics_config(std::nullopt, nowhere);
    std::filesystem::current_path(was);
    REQUIRE(config);
    CHECK(*config == GraphicsConfig{});
    // One asked for has to be there.
    CHECK_FALSE(find_graphics_config(std::filesystem::path{"missing/graphics.toml"}, nowhere));
}
