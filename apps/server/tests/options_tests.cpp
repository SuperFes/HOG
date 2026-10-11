#include <hog/app/options.hpp>

#include <catch2/catch_test_macros.hpp>

#include <string_view>
#include <vector>

using hog::app::parse_options;

TEST_CASE("server options default to the development config", "[app][options]") {
    const auto options = parse_options({});
    REQUIRE(options);
    CHECK(options->config == "config/server.toml");
    CHECK_FALSE(options->help);
}

TEST_CASE("server options are parsed", "[app][options]") {
    const std::vector<std::string_view> arguments{"--config", "/etc/hog/server.toml", "-h"};
    const auto options = parse_options(arguments);
    REQUIRE(options);
    CHECK(options->config == "/etc/hog/server.toml");
    CHECK(options->help);
    CHECK(parse_options(std::vector<std::string_view>{"--help"})->help);
}

TEST_CASE("bad server options are rejected", "[app][options]") {
    CHECK_FALSE(parse_options(std::vector<std::string_view>{"--config"}));
    CHECK_FALSE(parse_options(std::vector<std::string_view>{"--verbose"}));
    CHECK_FALSE(parse_options(std::vector<std::string_view>{"server.toml"}));
    CHECK_FALSE(hog::app::usage().empty());
}
