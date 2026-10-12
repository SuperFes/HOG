#include <hog/viewer/options.hpp>

#include <catch2/catch_test_macros.hpp>

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
    CHECK_FALSE(options->help);
}

TEST_CASE("viewer options are read", "[viewer][options]") {
    const auto options = parsed({"--host", "localhost", "--port", "9000", "--width", "2048", "-h"});
    REQUIRE(options);
    CHECK(options->host == "localhost");
    CHECK(options->port == 9000);
    CHECK(options->width == 2'048);
    CHECK(options->help);
}

TEST_CASE("bad viewer options are rejected", "[viewer][options]") {
    CHECK_FALSE(parsed({"--port"}));
    CHECK_FALSE(parsed({"--port", "0"}));
    CHECK_FALSE(parsed({"--port", "65536"}));
    CHECK_FALSE(parsed({"--port", "77x"}));
    CHECK_FALSE(parsed({"--width", "10"}));
    CHECK_FALSE(parsed({"--colour", "red"}));
}
