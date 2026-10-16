#include <hog/client/options.hpp>
#include <hog/core/build_info.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <string_view>
#include <vector>

using hog::client::locate;
using hog::client::parse_options;
using hog::client::Place;
using Arguments = std::vector<std::string_view>;

TEST_CASE("client options default to the development planet", "[client][options]") {
    const auto options = parse_options({});
    REQUIRE(options);
    CHECK(options->planet == "config/planet.toml");
    CHECK_FALSE(options->start);
    CHECK_FALSE(options->time_of_day);
    CHECK(options->threads == 0);
    CHECK_FALSE(options->connect);
    CHECK_FALSE(options->login);
    CHECK_FALSE(options->name);
    CHECK(options->companion == "res/Matt.png");
    CHECK_FALSE(options->quit_after);
    CHECK_FALSE(options->capture);
    CHECK_FALSE(options->timings);
    CHECK_FALSE(options->software);
    CHECK(options->validate == hog::debug_build);
    CHECK(options->reload_shaders == hog::debug_build);
    CHECK_FALSE(options->help);
}

TEST_CASE("client options are parsed", "[client][options]") {
    const auto options = parse_options(Arguments{"--planet",
                                                 "other.toml",
                                                 "--at",
                                                 "-12.5,170,300",
                                                 "--time",
                                                 "18:30",
                                                 "--threads",
                                                 "2",
                                                 "--connect",
                                                 "127.0.0.1:7675",
                                                 "--login",
                                                 "127.0.0.1:4647",
                                                 "--name",
                                                 "Matt",
                                                 "--simulate",
                                                 "lag=75,loss=2",
                                                 "--companion",
                                                 "friend.png",
                                                 "--quit-after",
                                                 "3",
                                                 "--capture",
                                                 "out.png",
                                                 "--timings",
                                                 "--software",
                                                 "--validate",
                                                 "--reload-shaders",
                                                 "-h"});
    REQUIRE(options);
    CHECK(options->planet == "other.toml");
    CHECK(options->start == Place{.latitude = -12.5, .longitude = 170.0, .altitude = 300.0});
    CHECK(options->time_of_day == 18.5 / 24.0);
    CHECK(options->threads == 2);
    CHECK(options->connect == "127.0.0.1:7675");
    CHECK(options->login == "127.0.0.1:4647");
    CHECK(options->name == "Matt");
    CHECK(options->simulate ==
          hog::net::Simulation{.lag = std::chrono::milliseconds{75}, .loss = 2});
    CHECK(options->companion == "friend.png");
    CHECK(options->quit_after == std::chrono::seconds{3});
    CHECK(options->capture == "out.png");
    CHECK(options->timings);
    CHECK(options->software);
    CHECK(options->validate);
    CHECK(options->reload_shaders);
    CHECK(options->help);
}

TEST_CASE("a client can fly alone", "[client][options]") {
    CHECK(!parse_options(Arguments{"--alone"})->companion);
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
    CHECK_FALSE(parse_options(Arguments{"--time", "18"}));
    CHECK_FALSE(parse_options(Arguments{"--time", "24:00"}));
    CHECK_FALSE(parse_options(Arguments{"--time", "12:60"}));
    CHECK_FALSE(parse_options(Arguments{"--time", "noon"}));
    CHECK_FALSE(parse_options(Arguments{"--connect", "127.0.0.1:7675", "--simulate", "lag=x"}));
    CHECK_FALSE(parse_options(Arguments{"--simulate", "lag=75"}));
    CHECK_FALSE(hog::client::usage().empty());
}

TEST_CASE("paths not found here are found from above the program", "[client][options]") {
    const std::filesystem::path source{HOG_SOURCE_DIR};
    const std::filesystem::path found = locate("config/planet.toml", source / "apps/client/tests");
    CHECK(std::filesystem::equivalent(found, source / "config/planet.toml"));

    const std::filesystem::path absolute = source / "nowhere.toml";
    CHECK(locate(absolute, source) == absolute);
    CHECK(locate("nowhere/at/all.toml", source / "apps") == "nowhere/at/all.toml");
}

TEST_CASE("a client can be told not to validate", "[client][options]") {
    const auto options = parse_options(Arguments{"--validate", "--no-validate"});
    REQUIRE(options);
    CHECK_FALSE(options->validate);
}

TEST_CASE("a client can be told not to reload shaders", "[client][options]") {
    const auto options = parse_options(Arguments{"--reload-shaders", "--no-reload-shaders"});
    REQUIRE(options);
    CHECK_FALSE(options->reload_shaders);
}

TEST_CASE("logging in needs a name and a server to go on to", "[client][options]") {
    CHECK_FALSE(parse_options(Arguments{"--login", "127.0.0.1:4647", "--name", "Matt"}));
    CHECK_FALSE(
        parse_options(Arguments{"--login", "127.0.0.1:4647", "--connect", "127.0.0.1:7675"}));
    CHECK_FALSE(parse_options(Arguments{"--name", "Matt", "--connect", "127.0.0.1:7675"}));
}
