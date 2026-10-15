#include <hog/client/options.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <string_view>
#include <vector>

using hog::client::parse_options;
using hog::client::Place;
using Arguments = std::vector<std::string_view>;

TEST_CASE("client options default to the development planet", "[client][options]") {
    const auto options = parse_options({});
    REQUIRE(options);
    CHECK(options->planet == "config/planet.toml");
    CHECK_FALSE(options->start);
    CHECK(options->threads == 0);
    CHECK_FALSE(options->quit_after);
    CHECK_FALSE(options->capture);
    CHECK_FALSE(options->software);
    CHECK_FALSE(options->validate);
    CHECK_FALSE(options->help);
}

TEST_CASE("client options are parsed", "[client][options]") {
    const auto options = parse_options(Arguments{"--planet", "other.toml", "--at", "-12.5,170,300",
                                                 "--threads", "2", "--quit-after", "3", "--capture",
                                                 "out.png", "--software", "--validate", "-h"});
    REQUIRE(options);
    CHECK(options->planet == "other.toml");
    CHECK(options->start == Place{.latitude = -12.5, .longitude = 170.0, .altitude = 300.0});
    CHECK(options->threads == 2);
    CHECK(options->quit_after == std::chrono::seconds{3});
    CHECK(options->capture == "out.png");
    CHECK(options->software);
    CHECK(options->validate);
    CHECK(options->help);
}

TEST_CASE("bad client options are rejected", "[client][options]") {
    CHECK_FALSE(parse_options(Arguments{"--planet"}));
    CHECK_FALSE(parse_options(Arguments{"--fly"}));
    CHECK_FALSE(parse_options(Arguments{"--at", "1,2"}));
    CHECK_FALSE(parse_options(Arguments{"--at", "1,2,3,4"}));
    CHECK_FALSE(parse_options(Arguments{"--at", "91,0,0"}));
    CHECK_FALSE(parse_options(Arguments{"--at", "0,east,0"}));
    CHECK_FALSE(parse_options(Arguments{"--threads", "-1"}));
    CHECK_FALSE(parse_options(Arguments{"--threads", "many"}));
    CHECK_FALSE(parse_options(Arguments{"--quit-after", "1.5"}));
    CHECK_FALSE(hog::client::usage().empty());
}
