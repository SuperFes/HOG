#include <hog/viewer/frame.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <string>
#include <vector>

using namespace hog;
using hog::viewer::Frame;

TEST_CASE("a frame finds its layers by name", "[viewer][frame]") {
    const probe::Layers layers{.time = GameTime{std::chrono::hours{5}},
                               .layers = {probe::Layer{.name = "a", .values = {1.0F, 2.0F, 3.0F}},
                                          probe::Layer{.name = "short", .values = {1.0F}}}};
    const Frame frame{probe::Welcome{}, layers, 3};
    CHECK(frame.time() == GameTime{std::chrono::hours{5}});
    REQUIRE(frame.layer("a").size() == 3);
    CHECK(frame.layer("a")[2] == 3.0F);
    // Layers that don't cover every cell are as good as missing.
    CHECK(frame.layer("short").empty());
    CHECK(frame.layer("b").empty());
    CHECK(Frame{}.layer("a").empty());
}

TEST_CASE("a frame lists the species a server tracks", "[viewer][frame]") {
    const probe::Welcome welcome{
        .version = probe::protocol_version,
        .world_id = 1,
        .level = 5,
        .planet_radius = 1.0,
        .layers = {probe::LayerInfo{.name = "weather/temperature", .unit = "°C"},
                   probe::LayerInfo{.name = "wildlife/forage", .unit = "kg/km²"},
                   probe::LayerInfo{.name = "wildlife/deer", .unit = "animals"},
                   probe::LayerInfo{.name = "wildlife/deer/fed", .unit = ""},
                   probe::LayerInfo{.name = "wildlife/red fox", .unit = "animals"}}};
    const Frame frame{welcome, probe::Layers{}, 1};
    CHECK(frame.species() == std::vector<std::string>{"deer", "red fox"});
}
