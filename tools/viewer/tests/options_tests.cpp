#include <hog/core/build_info.hpp>
#include <hog/viewer/options.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <string_view>
#include <vector>

using hog::viewer::parse_options;

namespace {

auto parsed(std::vector<std::string_view> arguments) {
    return parse_options(arguments);
}

} // namespace

TEST_CASE("viewer options default to the local dev server", "[viewer][options]") {
    const auto options = parsed({});
    REQUIRE(options);
    CHECK(options->host == "127.0.0.1");
    CHECK(options->port == 7777);
    CHECK(options->width == 1'024);
    CHECK_FALSE(options->quit_after);
    CHECK_FALSE(options->capture);
    CHECK_FALSE(options->globe);
    CHECK_FALSE(options->software);
    CHECK(options->validate == hog::debug_build);
    CHECK_FALSE(options->help);
}

TEST_CASE("viewer options are read", "[viewer][options]") {
    const auto options =
        parsed({"--host", "localhost", "--port", "9000", "--width", "2048", "--quit-after", "3",
                "--capture", "out.png", "--overlay", "wildlife", "--species", "red fox", "--globe",
                "--software", "--validate", "-h"});
    REQUIRE(options);
    CHECK(options->host == "localhost");
    CHECK(options->port == 9000);
    CHECK(options->width == 2'048);
    CHECK(options->quit_after == std::chrono::seconds{3});
    CHECK(options->capture == std::filesystem::path{"out.png"});
    CHECK(options->overlay == hog::viewer::Overlay::Wildlife);
    CHECK(options->species == "red fox");
    CHECK(options->globe);
    CHECK(options->software);
    CHECK(options->validate);
    CHECK(options->help);
}

TEST_CASE("bad viewer options are rejected", "[viewer][options]") {
    CHECK_FALSE(parsed({"--port"}));
    CHECK_FALSE(parsed({"--port", "0"}));
    CHECK_FALSE(parsed({"--port", "65536"}));
    CHECK_FALSE(parsed({"--port", "77x"}));
    CHECK_FALSE(parsed({"--width", "10"}));
    CHECK_FALSE(parsed({"--colour", "red"}));
    CHECK_FALSE(parsed({"--quit-after", "-1"}));
    CHECK_FALSE(parsed({"--capture"}));
    CHECK_FALSE(parsed({"--overlay", "rainbows"}));
}

TEST_CASE("a viewer can be told not to validate", "[viewer][options]") {
    const auto options = parsed({"--validate", "--no-validate"});
    REQUIRE(options);
    CHECK_FALSE(options->validate);
}
